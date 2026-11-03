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
- Not a general `proxy`: no owning storage, no small-buffer optimisation, no `dynamic_cast` equivalent, no formatting support. Those are added only when a second user needs them.

## Design

### The facility

It lives in `ustl`, beside `ustl::Function` (`system/lib/ustl/include/ustl/function/function.hpp`), which already is this pattern for one signature: a per-target-type `Ops` table built at compile time, no allocation, and an empty state that is a table whose thunk traps, so the call path has no branch. The new facility generalises it from one call to several, and from owning inline storage to a non-owning view.

| Object | What it is |
|---|---|
| Dispatch | A stateless constant naming an operation and how a concrete type answers it. A captureless generic lambda used as a non-type template argument, so no macro is needed. It may carry a fallback. |
| Convention | A dispatch together with the signature the erased call has: `Convention<dispatch, auto(RunQueue &, Thread *, EnqueueOptions) -> Status>`. |
| Facade | A list of conventions: the interface. A type models it when every dispatch is invocable on it with the convention's arguments and gives a result convertible to the convention's return type. |
| Table | One `constexpr` array of function pointers per (concrete type, facade), in `.rodata`. |
| View | `{void *object, Table const *table}`. Non-owning, trivially copyable, two pointers. `View<F>::of(object)` picks the table. An empty View points at a table whose entries trap. |

Events, in order, for one call: the caller holds a `View` it got from `Scheduler::schedule_of(discipline)`; `view.call<dispatch>(args...)` loads the function pointer for that dispatch from the table and calls it with the object pointer; the thunk casts the pointer back to the concrete type and runs the dispatch on it; the dispatch calls the member function, or, for an optional hook the type lacks, returns the default.

A sketch of the shape, not compiled:

```cpp
// A dispatch is a constant. The optional ones carry their default.
inline constexpr auto enqueue_thread = [](auto &d, auto &&...args) -> decltype(auto) {
    return d.enqueue_thread(std::forward<decltype(args)>(args)...);
};
inline constexpr auto yield_thread = [](auto &d, RunQueue &rq, Thread *t) -> void {
    if constexpr (requires { d.yield_thread(rq, t); }) {
        d.yield_thread(rq, t);
    }
};

using ScheduleFacade = ustl::Facade<
    ustl::Convention<enqueue_thread, auto(RunQueue &, Thread *, EnqueueOptions) -> Status>,
    ustl::Convention<yield_thread, auto(RunQueue &, Thread *) -> void>
    /* ... */>;

view.call<yield_thread>(rq, thread);
```

References. Linux `struct sched_class` (`kernel/sched/sched.h`, from memory, not opened) is the same idea written by hand: one table of function pointers per class, linked in priority order. Zircon has no counterpart: `Scheduler` (`zircon/kernel/include/kernel/scheduler.h:83-85`, checked in the local checkout) is one class that "implements fair and deadline scheduling algorithms", which is the shape this project chose not to follow in 260918. In-tree, `ustl::Function::Ops` is the closest precedent.

### Applied to the scheduler

- **The facade.** `ScheduleFacade` has one convention per `ISchedule` entry point except `discipline()`. The five pure ones become required. The others, which are virtual with a default body today, become optional: the default moves, unchanged, into the dispatch (a no-op for `yield_thread`, `put_prev_thread`, `set_next_thread`, `update_thread`, `on_thread_init`, `on_thread_dead` and `on_thread_unpark`; `NotSupported` for `yield_to_thread`; `Unimplemented` for `unpark_preempt` and `override_thread_base_profile`; `INVALID_CPU_NUM` for `select_target_cpu`; `Unsupported` for `migrate_thread`).
- **The registry.** `Scheduler::schedule_of(Discipline)` is a `switch` over the closed enum that returns the view of the discipline's static instance. Normal gives whichever of RoundRobin and Fair `SCHED_NORMAL_DISCIPLINE_FAIR` selects, the way `NormalRunQueue` is chosen today. Deadline gives an empty view that traps, which is what the debug assertion in `get_schedule()` does now. Adding a discipline is one enumerator, one case and one class. Gone: `ISchedule`, `Schedule<>`, `ScheduleEntry`, `.task.schedules` and its `KEEP` in `kernel.ld`, `Scheduler::m_schedules`, `set_schedule()` and `get_schedule()`, and the virtual `discipline()`.
- **No per-thread pointer.** `SchedEntity::m_schedule` is removed. Each call looks the view up by `ctx.discipline()`, an index into a two-element array of views. That removes the null-before-bind failure instead of guarding it.
- **Thread-safety analysis.** Two probes with the kernel's clang (scratchpad, not in the repo) show that `-Wthread-safety` checks a plain function call and a virtual call, does not check a call through a function pointer or a table (the attribute is ignored on a function-pointer type), and does not connect a lambda-based visitor to its dispatcher, because the lambda's captured `rq` and the dispatcher's parameter `rq` are different expressions to the analysis. So the erased hop is unchecked. The discipline functions keep `TA_REQUIRES(rq.lock())` and their bodies stay checked, and `Scheduler` gets one thin private function per entry point that is itself `TA_REQUIRES(rq.lock())` and makes the single erased call, so every caller of the scheduler is still checked. The unchecked part is that one line per entry point.
- **Disciplines.** `RoundRobin` stops deriving from `Schedule<>` and drops `override`; it keeps its member functions and stays a stateless object.
- **Fair.** `fair.hpp` and `fair.cpp` need the same mechanical adaptation (drop the base, drop `override`). They are out of the build and are the user's own, so this design does not touch them; the adaptation is written as a list for the user, as section 5 of 260918 did.

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
| D3 Scope | non-owning view only / also owning inline storage | View only | None | |
| D4 Names | Facade, Convention, View / Rust-style `Dyn` | Facade, Convention, View | None | |
| D5 Writing a dispatch | captureless lambda as a non-type template argument / macro | Lambda | None | |
| D6 Per-thread pointer | remove `SchedEntity::m_schedule` / keep a View per thread | Remove | None | |
| D7 Required entry points | the five pure ones / more / fewer | The five that are pure today | None | |
| D8 The empty slot (Deadline) | trap as `Function` does / answer `NotSupported` | Trap | None | |
| D9 Fair's adaptation | list for the user / done here | List for the user | None | |

## Testing and verification

- **The facility.** Host tests in `ustl` with gtest: two unrelated types behind one facade; an optional hook answered by the type's own function and by the default; a required one missing is a compile error (`static_assert` on the modelling concept); `View` is trivially copyable and no larger than two pointers; an empty view traps. A translation unit built with the kernel's flags (`-ffreestanding -fno-exceptions -fno-rtti`) is the real test of the freestanding claim, and the scheduler is it.
- **The scheduler.** The behaviour must not change, so the check is `verify` (both images and the host tests), the kernel tests that exercise scheduling (`thread_join.test`, `waitqueue.test`, `pagereclaim.test` as a user of preemption), and a four-CPU boot, as the 260918 sequence did. `clang-tidy` on every touched file.
- **Fair.** After the user's adaptation, `-fsyntax-only` on `fair.cpp`, the way `docs/changes/260917/kernel-task-sched.md` does it.
- **Not verifiable yet.** Performance. No claim is made, and one would need a before-and-after under the performance workflow.

## Open questions

- How many disciplines the user expects the tree to hold. The design does not depend on the number, but D8 and the cost of the lookup do.
- Whether other subsystems that use a table of operations (the interrupt `ControllerOps`, the device-driver families) should move onto the facility later. Out of scope here; the facility is written so that they could.
- The shape of the first commit: the facility with its host tests alone, before the scheduler uses it.
