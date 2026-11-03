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

## Title: the dispatch macros exempt the dispatch from the thread-safety analysis (2026-11-03)

**Files**
- `system/lib/ustl/include/ustl/dyn.hpp` — `[[clang::no_thread_safety_analysis]]` on the call operator `USTL_DYN_MEMBER` and `USTL_DYN_MEMBER_OR` write; the two `FnList` constructor parameters renamed `first` and `rest`

**Symptom**
The first scheduler built on `Dyn` failed to compile with eight errors, one per required method, such as `calling function 'enqueue_thread' requires holding SpinLock 'forward(args).m_rqlock'`, pointing at the macro-written dispatch.

**Cause**
A dispatch is a forwarding layer: it calls `self.member(std::forward<Args>(args)...)` and cannot know which argument names the lock that `member` requires (`TA_REQUIRES(rq.lock())`). As soon as a type's member carries that annotation, the analysis looks into the dispatch, finds no lock held there, and cannot match `rq.lock()` with `forward(args)`, so it reports every dispatch of an annotated member. There is nothing the dispatch could declare in general. The lock has to be checked before the erased call, in a function that has the argument by name. `clang-tidy` also flagged `head_` and `tail_`, parameter names with a trailing underscore, in the commit before.

**Change**
The macros put `[[clang::no_thread_safety_analysis]]` on the call operator, and say why in their comment. The check moved to the scheduler's `call_schedule<>()` (`docs/changes/261103/kernel-task-sched.md`). The parameters are `first` and `rest`.

**Reference**
None, this project's own design.

**Verification**
The scheduler change that needed it builds with `-Werror -Wthread-safety`; `dyn-test` passes; see `docs/changes/261103/kernel-task-sched.md`.

**Deliberately not done**
A parameter of the macro to say which argument is the lock. It would be a second, unchecked way to say what `TA_REQUIRES` says on the member.

**How it was found**
The build of the scheduler change; the eight errors named the dispatch.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why can a forwarding dispatch not carry the lock requirement of the member it calls?
- Where does a caller of a `Ref` have to show it holds a lock, and why there?
- What would a parameter naming the lock argument add, and what would it cost?

## Title: ustl::Own, the owning counterpart of Ref (2026-11-03)

**Files**
- `system/lib/ustl/include/ustl/dyn.hpp` — `Own<Dyn, Size, Align>`; `Method::thunk` launders the pointer it is given; `Ref` gets a private constructor and `Own` as a friend
- `system/lib/ustl/test/dyn_test.cpp` — four tests of `Own` and the `static_assert`s of what fits
- `.agents/tasks/sched-facade/DESIGN.md` — `Own` described as built

**Symptom**
None. The user asked for an owning type beside `Ref` (the answer to the scope question of 2026-11-03), and the scheduler does not use it.

**Cause**
**A `Ref` needs a table of methods; an object that has to move and die needs two more things the table does not have, how to relocate and how to destroy.** So `Own` is `{Ops const *, storage}`, where `Ops` is `{the Ref's own method table, relocate, destroy}`, one per held type, in read-only data. Everything else follows from the object living in the `Own`.

1. **The buffer is in the type.** `Own<Dyn, Size, Align>` holds `Size` bytes aligned to `Align`, three pointers by default, as `ustl::Function` does. `Function` fixes the size; `Own` makes it a parameter, because the objects behind an interface are not all as small as a callable's capture list, and a different size is a different type, as with any buffer. A type that does not fit is a compile error at the line that built it, not a trip to the heap: the `FitsInline` concept makes "does not fit" part of overload resolution, so `std::is_constructible_v` says no and the compiler reports the clause that failed.
2. **The held type's move must not throw.** Moving an `Own` is relocation: construct at the destination from the source, then destroy the source. A move that could throw would leave the object in neither place, so a type with a throwing move does not construct an `Own` (the same rule as `Function`), and `relocate` is `noexcept`.
3. **The empty state is a table, not a null pointer.** An empty `Own` points at an `Ops` whose methods trap and whose `relocate` and `destroy` do nothing, so moving, resetting and destroying an empty one need no branch and calling one stops at once. `operator bool` compares the pointer.
4. **`launder`.** The method thunk is given the address of the storage, not the pointer `std::construct_at` returned, and a pointer to bytes is not a pointer to the object built in them until `std::launder` says so. `Function::target` does the same. The thunk is shared with `Ref`, so it launders for both; for a `Ref` the pointer already points at an object and the call does nothing.
5. **Value semantics, so `call` is not const.** A `Ref` is a reference, and calling through a const reference may still change the object it refers to. An `Own` holds the object, and a method that changes it changes the `Own`.

`ref()` lends a `Ref` to the held object. It is good until the `Own` is moved from, reset or destroyed, which the comment says; it is built through a private constructor that `Own` is a friend of, so a `Ref` cannot be made from a bare pointer elsewhere.

**Change**
```cpp
ustl::Own<Schedule> own{RoundRobin{}};            // the object lives in the Own
ustl::Own<Schedule> placed{std::in_place_type<RoundRobin>};   // built in place
own.call<Enqueue>(rq, thread, options);
ustl::Ref<Schedule> view = own.ref();             // lent; dies with the Own's object
ustl::Own<Schedule> moved = std::move(own);       // relocates; `own` is empty
```

Rejected: a heap fallback for what does not fit (the reason `Function` has none), a copyable `Own` (it would demand that every held type be copyable), and a shared `Own` (no user for it).

**Reference**
In-tree, `ustl::Function` (`system/lib/ustl/include/ustl/function/function.hpp`): the `Ops` table with `invoke`, `relocate` and `destroy`, the empty table, `FitsInline`, and `launder` in `target()`. Outside, EnTT `basic_poly<Concept, Size, Align>` (the same inline-buffer parameters, read in its documentation) and Microsoft `proxy`'s `make_proxy_inplace`.

**Verification**
- Built and tested in a clean worktree (`.worktrees/verify-own`, HEAD `72c9b361` plus the two files). `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 14.9 s; dbg PASS, 46 passed, 5 skipped, 0 failed; rel PASS, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 164 passed. `waitqueue.test`, `threadjoin.test`, `preempt.test`, `dpc.test`, `timer.test`, `timershare.test` and `hotplug.test` ok on the dbg image with four CPUs, since the scheduler is built on `Method::thunk`, which changed.
- A scheduler-shaped probe that builds an `Own` from a class, calls it and moves it compiles with the flags of `scheduler.cpp` (`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti -std=c++2c`); `sizeof(Own)` is four words.
- Mutations of the header against the host test, each caught by `test_own_destroys_what_it_holds_exactly_once`: `destroy_target` destroying nothing (`live == 1` fails), a move that leaves the source non-empty (`!a` fails), and a relocation that does not destroy the source (`live == 1` fails).
- `static_assert`s: not copyable, nothrow movable; accepts a class, a copy of one and an in-place construction; refuses a type that does not model the interface, one too big, one over-aligned and one with a throwing move; accepts the big and the over-aligned ones in a larger buffer.
- `scripts/format_cpp.py --check` clean; `clang-tidy` on `scheduler.cpp` (which includes the header) reports nothing in `dyn.hpp`.
- Not verified: the empty `Own` trapping (a death test, and the thunk is one line); that the `launder` is needed rather than prudent, which is a question for the language lawyer and not for a test; an `Own` used by anything but its test.

**Deliberately not done**
- Nothing uses it. The scheduler holds its disciplines as `Ref`s to static objects, which is what a stateless discipline wants.
- No conversion between `Own`s of different sizes, no `swap`, no `emplace` on a live `Own` (`reset()` and construct another), no `make<T>` beside the two constructors.
- No allocation, and no shared ownership.

**How it was found**
By the user's answer to the scope question: both a non-owning reference and an owning type.

**The user's prediction**
None

**Three questions you should be able to answer**
- What does an `Own` need in its table that a `Ref` does not, and why?
- Why does a type with a throwing move constructor not construct an `Own`, and what would break if it did?
- Why does the method thunk call `std::launder`, and why is that harmless for a `Ref`?

