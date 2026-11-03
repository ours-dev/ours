# sched-facade: discipline dispatch through a ustl facade instead of ISchedule's virtuals

**Date**: 2026-11-03
**Status**: Proposed

## Problem

A discipline is reached through `ISchedule` (`kernel/include/ours/task/sched/schedule.hpp:89-187`): eighteen virtual entry points, of which six (`enqueue_thread`, `dequeue_thread`, `evaluate_next_thread`, `on_thread_tick`, `num_queued`, `discipline`) are pure and the rest carry runtime defaults such as `Status::Unimplemented`. Each discipline derives from `Schedule<Derived, Discipline>`, a CRTP base that registers one shared stateless instance in the linker section `.task.schedules` (`kernel/kernel.ld:239-248`). `Scheduler` reads that section once per CPU into `m_schedules`, binds each thread to its `ISchedule *` in `set_schedule()`, and calls through `SchedEntity::m_schedule` at about fourteen places in `scheduler.cpp`.

The code's own comments record what this costs:

- Three traps in the registry that were all stepped in (`schedule.hpp:222-240`): the section name the linker script's catch-all swallows, `FORCE_USED` not being enough under `--gc-sections`, and the static data member that is instantiated only when odr-used.
- A thread's `m_schedule` is null until `set_schedule()` runs, and a call through it took an instruction-fetch fault at address 0 (`sched_entity.hpp:305-310`, `scheduler.cpp` near line 182).
- `RunQueue` had to be sealed behind a single friend so that a discipline could not reach into the generic accounting, and every parameter of every virtual had to be named so `TA_REQUIRES(rq.lock())` can be written at all (`schedule.hpp:77-88`).
- The registry's whole point is that adding a discipline does not touch `Scheduler`, and `docs/design/260918/kernel-task-sched.md` section 1 records that it does.

The user's call of 2026-11-03: the subsystem is rough, `virtual` is unwanted where it can be avoided, and the replacement should be a modern type-erasure facility in the style of Microsoft's `proxy` facades or EnTT's `poly`, not a hand-written table of function pointers. The premise of `docs/design/260918/kernel-task-sched.md` stands: `Discipline` will grow, and the model is Linux's `sched_class`, not Zircon's single scheduler.

## Goals and non-goals

Goals:

- No `virtual` and no CRTP base in the scheduler's discipline seam: a discipline is a plain class with the member functions it has.
- The interface is checked at compile time: a discipline that lacks a required entry point does not compile, and an optional one is answered by a default written once.
- The registry leaves the linker script. The set of disciplines is one function that names them.
- No allocation, and no dependence on exceptions, RTTI or a hosted C library: the facility builds under the kernel's `-ffreestanding -fno-exceptions -fno-rtti`.
- The scheduler behaves as it does now.

Non-goals:

- No speedup is claimed. The call is still one indirect call through a table, as a virtual call is. If a measurement is wanted it goes through the performance workflow of CLAUDE.md, before and after.
- `fair.*` and every scheduling algorithm stay as they are.
- Not a general `proxy`: no shared or weak ownership, no run-time casts, no formatting support. Owning storage is in scope (the user's call) as `Own`, in a commit of its own after `Ref`, which is all the scheduler needs.

## Design

### The facility

It lives in `ustl`, beside `ustl::Function` (`system/lib/ustl/include/ustl/function/function.hpp`), which already is this pattern for one signature: a per-target-type `Ops` table built at compile time, no allocation, and an empty state that is a table whose thunk traps, so the call path has no branch. The new facility generalises it from one call to several, and from owning inline storage to a non-owning view.

| Object | What it is |
|---|---|
| Dispatch | A type with a static call operator that takes the object first and says how a concrete type answers one operation, a fallback included. Declared with `USTL_DYN_MEMBER` or `USTL_DYN_MEMBER_OR`, which write that struct and nothing else (the user's call to allow a macro here, 2026-11-03). |
| `Method<Dispatch, Signature>` | A dispatch together with the signature the erased call has: `Method<Enqueue, auto(RunQueue &, Thread *, EnqueueOptions) -> Status>`. |
| `Dyn<Method...>` | The interface: a list of methods. A type models it when every dispatch is invocable on it with the method's arguments and gives a result convertible to the method's return type. Two methods with the same dispatch do not compile. |
| Table | One `constexpr` list of function pointers per (concrete type, interface), in `.rodata`. |
| `Ref<Dyn>` | `{void *object, Table const *table}`. Non-owning, trivially copyable, two pointers. Built from an `T &` that models the interface. A default-constructed `Ref` points at a table whose entries trap. |
| `Own<Dyn>` | The owning counterpart, to come in a commit of its own: the object lives inside the `Own`, in a fixed inline buffer, as `ustl::Function` keeps its target. Move-only, never allocates, an empty state that traps. It is not needed by the scheduler. |

Events, in order, for one call: the caller holds a `Ref` it got from `Scheduler::schedule_of(discipline)`; `ref.call<Enqueue>(args...)` loads the function pointer for that method from the table and calls it with the object pointer; the thunk casts the pointer back to the concrete type and runs the dispatch on it; the dispatch calls the member function or, for an optional method the type lacks, the fallback.

Built in the first commit (`ustl/dyn.hpp`, `ustl/test/dyn_test.cpp`), and written the way the scheduler will use it:

```cpp
USTL_DYN_MEMBER(Enqueue, enqueue_thread);
USTL_DYN_MEMBER_OR(Yield, yield_thread, ustl::dyn::Ignore{});
USTL_DYN_MEMBER_OR(YieldTo, yield_to_thread, ustl::dyn::Returns<Status::NotSupported>{});

using Schedule = ustl::Dyn<
    ustl::Method<Enqueue, auto(RunQueue &, Thread *, EnqueueOptions)->Status>,
    ustl::Method<Yield, auto(RunQueue &, Thread *)->void>,
    ustl::Method<YieldTo, auto(RunQueue &, Thread *, Thread *)->Status>>;

ustl::Ref<Schedule> schedule = round_robin;
schedule.call<Enqueue>(rq, thread, options);
```

What the first commit established, with the kernel's own flags (`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti -std=c++2c`, taken from the compile command of `scheduler.cpp`): the header compiles; a `Ref` is two pointers; an erased call compiles at `-O2` to moving the arguments into place and `jmpq *(%rax)`, one indirect tail call through the table entry. A virtual call loads the vptr from the object first and then the function from the vtable; here the table pointer is already in the `Ref`, so the call has one dependent load fewer. That is read off the assembly and not measured.

References. Linux `struct sched_class` (`kernel/sched/sched.h`, from memory, not opened) is the same idea written by hand: one table of function pointers per class, linked in priority order. Zircon has no counterpart: `Scheduler` (`zircon/kernel/include/kernel/scheduler.h:83-85`, checked in the local checkout) is one class that "implements fair and deadline scheduling algorithms", which is the shape this project chose not to follow in 260918. In-tree, `ustl::Function::Ops` is the closest precedent.

### Applied to the scheduler

- **The interface.** `Schedule`, a `Dyn`, has one method per `ISchedule` entry point except `discipline()`. The five pure ones become required. The others, which are virtual with a default body today, become optional: the default moves, unchanged, into the declaration of the method as its fallback (a no-op for `yield_thread`, `put_prev_thread`, `set_next_thread`, `update_thread`, `on_thread_init`, `on_thread_dead` and `on_thread_unpark`; `NotSupported` for `yield_to_thread`; `Unimplemented` for `unpark_preempt` and `override_thread_base_profile`; `INVALID_CPU_NUM` for `select_target_cpu`; `Unsupported` for `migrate_thread`).
- **The registry.** `Scheduler::schedule_of(Discipline)` is a `switch` over the closed enum that returns a `Ref` to the discipline's static instance. Normal gives whichever of RoundRobin and Fair `SCHED_NORMAL_DISCIPLINE_FAIR` selects, the way `NormalRunQueue` is chosen today. Deadline gives an empty `Ref`, which traps, which is what the debug assertion in `get_schedule()` does now. Adding a discipline is one enumerator, one case and one class. Gone: `ISchedule`, `Schedule<>`, `ScheduleEntry`, `.task.schedules` and its `KEEP` in `kernel.ld`, `Scheduler::m_schedules`, `set_schedule()` and `get_schedule()`, and the virtual `discipline()`.
- **No per-thread pointer.** `SchedEntity::m_schedule` is removed. Each call looks the view up by `ctx.discipline()`, an index into a two-element array of `Ref`s. That removes the null-before-bind failure instead of guarding it.
- **Thread-safety analysis.** Two probes with the kernel's clang (scratchpad, not in the repo) show that `-Wthread-safety` checks a plain function call and a virtual call, does not check a call through a function pointer or a table (the attribute is ignored on a function-pointer type), and does not connect a lambda-based visitor to its dispatcher, because the lambda's captured `rq` and the dispatcher's parameter `rq` are different expressions to the analysis. So the erased hop is unchecked. The discipline functions keep `TA_REQUIRES(rq.lock())` and their bodies stay checked, and `Scheduler` gets one thin private function per entry point that is itself `TA_REQUIRES(rq.lock())` and makes the single erased call, so every caller of the scheduler is still checked. The unchecked part is that one line per entry point.
- **Disciplines.** `RoundRobin` stops deriving from `Schedule<>` and drops `override`; it keeps its member functions and stays a stateless object.
- **Fair.** Out of the build and the user's own, so it is not touched, and it did not build before this change either (it still says `Runtime`, `EnqueueControl` and `DequeueControl`, which were renamed `RunQueue`, `EnqueueOptions` and `DequeueOptions`). What bringing it back now needs, for the user to do as section 5 of 260918 did:
  - `class Fair` is defined inside `fair.cpp`. `Scheduler` names it in `scheduler.cpp` (`using NormalSchedule = Fair` under `SCHED_NORMAL_DISCIPLINE_FAIR`), so its definition has to be in `fair.hpp`.
  - Drop the base: `class Fair final: public Schedule<Fair, Discipline::Normal>` becomes `class Fair final`, and every `override` goes.
  - Delete `using ISchedule::migrate_thread;` and `using ISchedule::select_target_cpu;`. Fair's functions of those names take other parameters, so the methods are answered by their defaults (`Status::Unsupported`, `INVALID_CPU_NUM`) until Fair has ones of the right shape.
  - Delete `template class Schedule<Fair, Discipline::Normal>;` and the comment above it, and reword the two comments that name `Schedule<>::REGISTRATION` and `ISchedule` (`fair.cpp` near line 95, `fair.hpp` near line 24).
  - Optional: declare `TA_REQUIRES(rq.lock())` on the functions called under the run queue lock, as `RoundRobin` now does, so that their bodies are checked.

## Alternatives considered

- **Keep `virtual` and tidy the registry.** Smallest change, and the user does not want `virtual`. Kept as the baseline.
- **Static dispatch by `switch` at every call (option B of the 2026-11-03 discussion).** Cheapest at run time and fully checked by the analysis. Rejected because `Discipline` is expected to grow: every new discipline adds a case at about fourteen call sites.
- **A hand-written `constexpr` table of function pointers per discipline (Linux's way).** Rejected by the user in favour of a facility. Eighteen pointers per discipline written by hand is also where a transposed pair of same-typed parameters goes unnoticed, as `Fair::unpark_preempt()` did (`docs/repairs/260917/kernel-task-sched.md` item 2).
- **Import Microsoft `proxy`.** Probed on 2026-11-03: `proxy.h` at commit `dc3d95c` (2026-01-29) does not compile with the kernel's real flags (`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti -std=c++2c`). It includes `<cstdlib>`, and its no-exceptions path is `std::abort()`; libc++'s `stdlib.h` wants the C library's `ldiv_t` and `abort`, which the kernel does not have. It would need a shim or a patch, and `vendor-import` says kernel code does not take third-party dependencies through the vendor path. The user chose an in-tree facility.
- **EnTT `poly`.** Built on `entt::any`; the documentation says nothing about freestanding or exception-free builds, and it is part of an ECS library. Not pursued.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 Direction | virtual and tidy / static switch / table with a facility | Table with a facility | None | Facility, 2026-11-03 (first answered "static switch", changed after the 260918 premise was shown) |
| D2 Where it lives | `ustl` / vendored `proxy` / EnTT | `ustl` | None | `ustl`, 2026-11-03 |
| D3 Scope | non-owning reference only / also owning inline storage | Reference only, owning later | None | Both, 2026-11-03 (`Ref` first, `Own` in a later commit) |
| D4 Names | Facade, Convention, View / Dyn, Method, Ref | Facade, Convention, View | None | `Dyn`, `Method`, `Ref`; the owning type `Own`, 2026-11-03 |
| D5 Writing a dispatch | macro / captureless lambda as a non-type template argument | Lambda | None | Macro (`USTL_DYN_MEMBER`, `USTL_DYN_MEMBER_OR`), 2026-11-03 |
| D6 Per-thread pointer | remove `SchedEntity::m_schedule` / keep a reference per thread | Remove | None | Remove, 2026-11-03 |
| D7 Required entry points | the five pure ones / more / fewer | The five that are pure today | None | |
| D8 The empty slot (Deadline) | trap as `Function` does / answer `NotSupported` | Trap | None | |
| D9 Fair's adaptation | list for the user / done here | List for the user | None | |

## Testing and verification

- **The facility.** `ustl/test/dyn_test.cpp`, in the style of `function_test.cpp` (plain `assert`, one `main`): two unrelated types behind one interface and the objects themselves changing; a reference as a result; an optional method answered by the type's own function and by the fallback; `static_assert`s that a type lacking a required method, or a const object, does not model the interface, that a call naming no method of the interface or with the wrong arguments does not exist, and that a `Ref` is two pointers and trivially copyable; the empty `Ref` and rebinding. Two mutations of the header were tried: an optional method always answering with its fallback fails an assertion, and a `Ref` that never takes its object's table is caught at compile time. Not tested: that the empty `Ref` traps (it would need a death test, and the thunk is one line). The freestanding claim was checked by compiling a scheduler-shaped probe with the kernel's own flags, once; the scheduler itself will be the standing test.
- **The scheduler.** The behaviour must not change, so the check is `verify` (both images and the host tests), the kernel tests that exercise scheduling (`thread_join.test`, `waitqueue.test`, `pagereclaim.test` as a user of preemption), and a four-CPU boot, as the 260918 sequence did. `clang-tidy` on every touched file.
- **Fair.** After the user's adaptation, `-fsyntax-only` on `fair.cpp`, the way `docs/changes/260917/kernel-task-sched.md` does it.
- **Not verifiable yet.** Performance. No claim is made, and one would need a before-and-after under the performance workflow.

## Open questions

- How many disciplines the user expects the tree to hold. The design does not depend on the number, but D8 and the cost of the lookup do.
- Whether other subsystems that use a table of operations (the interrupt `ControllerOps`, the device-driver families) should move onto the facility later. Out of scope here; the facility is written so that they could.
- The shape of the first commit: the facility with its host tests alone, before the scheduler uses it.
