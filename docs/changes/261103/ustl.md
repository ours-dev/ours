# ustl changes

## Title: ustl::Dyn, Method and Ref, dynamic dispatch through a table kept beside the reference (2026-11-03)

**Files**
- `system/lib/ustl/include/ustl/dyn.hpp` — `Method`, `Dyn`, `Ref`, the fallbacks `dyn::Ignore` and `dyn::Returns`, and the macros `USTL_DYN_MEMBER` and `USTL_DYN_MEMBER_OR`
- `system/lib/ustl/test/dyn_test.cpp` — the host test, in the style of `function_test.cpp`
- `system/lib/ustl/BUILD.gn` — the header in `:headers`, the `dyn-test` executable, and its entry in the test group
- `.agents/tasks/sched-facade/DESIGN.md` — the decisions this change settled (D3 to D6), and what it established
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the two macros as a sanctioned exception to "no macros for logic"

**Symptom**
None. This adds a facility; nothing used it yet. What it is for is the scheduler: `ISchedule` has eighteen virtual entry points, a CRTP base that registers each discipline in a linker section, and a per-thread pointer that is null until it is bound (`.agents/tasks/sched-facade/DESIGN.md`, Problem). The user does not want `virtual` there and asked for a facility in the style of a facade or `poly`.

**Cause**
**A virtual call and a call through this facility are the same thing, one indirect call through a table of function pointers; what differs is where the table is kept.** A virtual function keeps it in the object, so the object has to carry a vptr, the type has to derive from the class that declares the function, and an interface cannot be written for a type that already exists. Here the table sits next to the reference: a `Ref` is `{object pointer, table pointer}`, the table is built at compile time for the pair (concrete type, interface) and placed in read-only data, and the object is an ordinary class that knows nothing of the interface. That is what takes the CRTP base and the `override`s out of a discipline.

Four things in the design are the kind that look arbitrary until they are tried.

1. **A dispatch is a type, not a lambda.** The table needs, per method, a function that casts the erased pointer back and calls the object's answer. The answer is "call the member named `enqueue_thread`", and naming a member is the one thing a template cannot do: a captureless lambda per method does it, but a lambda written as a non-type template argument needs a named constant to hold it, which is more to write than the struct the macro makes. `USTL_DYN_MEMBER(Enqueue, enqueue_thread)` writes a struct with a static call operator, and the type is the method's name everywhere after: `Method<Enqueue, ...>` and `ref.call<Enqueue>(...)`. The user allowed a macro for exactly this, and the skill now says so.
2. **An optional method carries its fallback in its declaration.** `USTL_DYN_MEMBER_OR(Yield, yield_thread, ustl::dyn::Ignore{})` calls the member if the type has one and the fallback if not, so every type models the method and the default is written once, where the method is declared, not in eighteen virtual bodies. This is also why the dispatch of an optional method returns `decltype(auto)` while a required one has a trailing return type that makes it fail to match instead.
3. **The table is a recursive struct, not a tuple.** `detail::FnList<F1, F2, ...>` holds one pointer and the rest. It is a literal type without a library header, so the table can be a `constexpr` variable and `<tuple>` need not be freestanding.
4. **`call` has a spelled return type and a constraint.** The first version returned `decltype(auto)`. Then `requires { ref.call<Bump>(); }`, with `Bump` taking an argument, was not false but a compile error from inside `call`'s body, because the body has to be instantiated to deduce the type, so the test of "a call with the wrong arguments does not exist" could not even be written. Now the return type is the method's declared one and the constraint is `std::is_invocable_v<Thunk, void *, CallArgs...>`.

A small trap that came with the macros: a dispatch defined in an anonymous namespace and listed in no interface is an unused function template with internal linkage, and `-Werror -Wunused-template` refuses it. The test defines its one deliberately unused dispatch in a named namespace.

**Change**
The facility, as `ustl/dyn.hpp`:

```cpp
USTL_DYN_MEMBER(Enqueue, enqueue_thread);
USTL_DYN_MEMBER_OR(Yield, yield_thread, ustl::dyn::Ignore{});

using Schedule = ustl::Dyn<
    ustl::Method<Enqueue, auto(RunQueue &, Thread *, EnqueueOptions)->Status>,
    ustl::Method<Yield, auto(RunQueue &, Thread *)->void>>;

ustl::Ref<Schedule> schedule = round_robin;     // any type that models Schedule
schedule.call<Enqueue>(rq, thread, options);
```

`Dyn::MODELED_BY<T>` says whether a type answers every method (a const object never does, since the `Ref` holds a mutable pointer); a `Ref` can only be built from a type that does. A default-constructed `Ref` points at a table whose entries trap, so calling nothing stops at once, the way `ustl::Function` does; its address is what `operator bool` compares.

Rejected: importing Microsoft `proxy` (see Verification: it does not compile with the kernel's flags), EnTT `poly` (no evidence of a freestanding build, and part of an ECS library), a hand-written table of function pointers per discipline (the user's call), and a lambda as the dispatch (point 1).

**Reference**
In-tree, `ustl::Function` (`system/lib/ustl/include/ustl/function/function.hpp`): the per-target `Ops` table built at compile time, no allocation, and an empty state that is a table whose thunk traps. `Dyn` generalises it from one call to several and from owning inline storage to a reference. Outside, Microsoft `proxy` (version 4, commit `dc3d95c`): `pro::facade_builder` with `add_convention` and `PRO_DEF_MEM_DISPATCH` for the interface and a member dispatch, `pro::proxy_view` for the non-owning handle. EnTT `entt::poly` for the concept-and-table idea. Linux `struct sched_class` (`kernel/sched/sched.h`, from memory, not opened) is the same table written by hand, which is what the scheduler will be compared with.

**Verification**
- Built and tested in a clean worktree (`.worktrees/verify-dyn`, HEAD `f8867c32` plus the three files of this change), because the main checkout's kernel build is held up by another session's uncommitted edits.
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 8.8 s; dbg PASS, shell finished at 5.9 s, 46 passed, 5 skipped, 0 failed; rel PASS, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 164 passed, 0 failed, and `//system/lib/ustl:dyn-test` is among them. The two images do not contain the facility yet, so what they add is that the new BUILD.gn target does not disturb the default build.
- Mutations of the header against the host test: an optional method that always answers with its fallback fails the assertion `counter.count == 0` in `test_optional_methods`; a `Ref` that never takes its object's table is rejected at compile time (the dispatches become unreachable and `-Wunused-template` refuses them), not by an assertion.
- The freestanding claim: a scheduler-shaped probe (three methods, two disciplines, one without the optional methods) compiled with the flags of `scheduler.cpp` taken from `compile_commands.json` (`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti -std=c++2c`): it compiles, and `sizeof(Ref)` is two pointers. At `-O2` the erased call is the argument shuffle and `jmpq *(%rax)`, one indirect tail call through the table entry. A virtual call loads the vptr from the object and then the function from the vtable, so this has one dependent load fewer. That is read off the assembly and not measured, and no speedup is claimed.
- `proxy` itself, same flags, same probe: `proxy.h` at `dc3d95c` does not compile. It includes `<cstdlib>`, its no-exceptions path is `std::abort()`, and libc++'s `stdlib.h` needs the C library's `ldiv_t` and `abort`, which the kernel does not have.
- `scripts/format_cpp.py --check` and `scripts/format_gn.py --check` are clean on the touched files. `clang-tidy` was not run.
- Not verified: that the empty `Ref` traps (a death test, for a one-line thunk); that a `Ref` is usable from the scheduler, which is the next step.

**Deliberately not done**
- `Own`, the owning counterpart. The user wants it in scope; it is a commit of its own with its own tests, since it brings relocation, destruction and an inline size, and the scheduler does not need it.
- The scheduler does not use the facility yet. `ISchedule` is untouched, and so is `fair.*`.
- No const reference, no run-time cast back to the concrete type, no `operator->` with the method names. Each can be added when something needs it.
- No `USTL_DYN_FREE` for a dispatch that calls a free function; a hand-written struct with a static call operator does that.

**How it was found**
By the design discussion of 2026-11-03 and two probes. A small clang program showed that `-Wthread-safety` checks a call to a function or a virtual member but not one through a function pointer or a table. A second showed that it does not connect a lambda-based visitor to its dispatcher, because the lambda's captured `rq` and the dispatcher's parameter `rq` are different expressions to the analysis. So the erased hop stays unchecked, and `Scheduler` will wrap each entry point in a function that is itself `TA_REQUIRES(rq.lock())`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Where does a virtual call keep its table, where does a `Ref` keep it, and what does that change about the type being called?
- Why is a dispatch declared as a type with a static call operator, and why does an optional method return `decltype(auto)` while a required one has a trailing return type?
- Why did `requires { ref.call<Bump>(); }` fail to compile in the first version, and what two changes make it a plain `false`?
