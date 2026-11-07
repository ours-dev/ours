# kernel-task-sched changes

## Call erased scheduler methods through named accessors (2026-11-07)

**Files**
- `system/lib/ustl/include/ustl/dyn.hpp` generates named member accessors for borrowed references.
- `system/lib/ustl/test/dyn_test.cpp` exercises named calls, reference results, optional fallbacks and an accessor obtained from a temporary handle.
- `kernel/include/ours/task/sched/schedule.hpp` declares which named methods require the supplied run queue's lock.
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/include/ours/task/sched/scheduler_inline.hpp` and `kernel/kernel/task/sched/scheduler.cpp` use the named methods and distinguish required lookup from optional lookup.

**Symptom**
Scheduler callers spelled an erased operation as a dispatch type passed to `call_schedule<>()` or `call_schedule_unlocked<>()`. They had to pick the wrapper that carried the operation's lock contract.

**Cause**
`Dyn` describes the erased signatures and `Ref` stores the object and table pointers. The dispatch macro originally provided only the thunk's expression for calling the concrete member. Clang cannot propagate a concrete method's lock requirement back through the function pointer, so the scheduler supplied the missing call-site check with an annotated forwarding template.

**Change**
Before:

```cpp
auto status = call_schedule<EnqueueThread>(so.discipline(), m_run_queue, thread, options);
```

After:

```cpp
auto const status = schedule_of(so.discipline())->enqueue_thread(m_run_queue, thread, options);
```

Each dispatch macro provides a named accessor with the facade's signature. `Ref::operator->` combines those accessors and carries a copy of the borrowed handle, which keeps calls through temporary handles valid and preserves the two-word `Ref`. A locked accessor's declaration names its first parameter in `TA_REQUIRES(first.lock())`. The run queue passed to the call identifies the CPU whose state the discipline can touch. Clang can match that lock to the caller's capability through `TA_RETURN_CAPABILITY`. Optional lookup uses `find_schedule()` and tests the returned reference. Required lookup keeps the debug assertion in `schedule_of()`.

**Reference**
Microsoft Proxy's `PRO_DEF_MEM_DISPATCH` and `proxy_view` establish the member-call shape and borrowed handle. This project's `Schedule` supplies the run-queue lock expressions. Zircon's `Scheduler` implements its scheduling algorithms directly in `zircon/kernel/include/kernel/scheduler.h`.

**Verification**
Changed-line clang-format passed. Clang-tidy ran on all six C++ files and reported no diagnostics on changed lines. The real scheduler compile passed with `-Wthread-safety -Werror`. The same compile rejected calls made with no lock and with a different run queue's lock. The Fair configuration passed syntax checks for `scheduler.cpp` and `fair.cpp`. The host suite passed 183 tests. QEMU dbg passed 46 shell checks with 5 skipped. QEMU rel passed 44 shell checks with 8 skipped and reached all 7 required markers. On four CPUs, ThreadJoin, Steal and Hotplug each passed. Preempt was skipped by its existing guard for the periodic balancer.

**Deliberately not done**
The scheduler's operation semantics and the behavior of the owning `ustl::Own` handle are outside the borrowed member-call change.

**How it was found**
The caller-facing style was identified by comparing the enqueue call site with a Proxy member call. A syntax-only probe using the kernel's compile flags established that an annotated accessor preserves lock checking before the erased dispatch.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why is the run queue the source of the capability in the accessor's annotation?
- How can an accessor call the erased function after the original `Ref` temporary has expired?
- Which lookup should a caller use when some disciplines may be unimplemented?

## Keep discipline lookup local to scheduler.cpp (2026-11-07)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` defines both lookups in its anonymous namespace and implements the two handover members.
- `kernel/include/ours/task/sched/scheduler.hpp` and `kernel/include/ours/task/sched/schedule.hpp` drop the lookup declarations and the reference to the old member function.
- `kernel/include/ours/task/sched/scheduler_inline.hpp` is removed and `kernel/kernel/task/sched/scheduler_pi.cpp` uses the existing Scheduler declarations.

**Symptom**
The two discipline lookup helpers were private members of Scheduler, with their declarations visible in every translation unit including the scheduler header.

**Cause**
The definitions of `put_prev_thread()` and `set_next_thread()` were in an inline header. Those definitions called `schedule_of()`, so the shared header needed its declaration even though callers in the PI implementation only needed the handover members.

**Change**
Before, the scheduler class declared the lookup.

```cpp
static auto schedule_of(Discipline discipline) -> ustl::Ref<Schedule>;
```

Now both lookups live beside the discipline objects in the anonymous namespace in `scheduler.cpp`. The handover definitions moved into the same file and retain `FORCE_INLINE` for the local scheduling path. The PI translation unit can call those members through their existing declarations.

```cpp
namespace {
    auto schedule_of(Discipline const discipline) -> ustl::Ref<Schedule> {
        auto const schedule = find_schedule(discipline);
        DEBUG_ASSERT(schedule, "No schedule for discipline[{}]", std::to_underlying(discipline));
        return schedule;
    }
}
```

**Reference**
None, this project's own design. The erased discipline lookup belongs to the translation unit that selects and calls the discipline.

**Verification**
Changed-line clang-format and clang-tidy passed for the surviving C++ files. The Fair configuration passed syntax checks for `scheduler.cpp` and `scheduler_pi.cpp`. QEMU dbg passed 46 shell checks with 5 skipped. QEMU rel passed 44 shell checks with 8 skipped and reached all 7 required markers. The host suite passed all 183 tests.

**Deliberately not done**
None

**How it was found**
A search for `schedule_of` and `find_schedule` showed that the shared handover definitions were the only lookup callers outside `scheduler.cpp`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did the inline header need to see the lookup declaration?
- Which names does the PI translation unit need after the move?
- How does an anonymous namespace restrict lookup linkage?

## Keep active scheduler selection local (2026-11-07)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` keeps the active-scheduler retry helper and the discipline count near their uses.
- `kernel/include/ours/task/sched/scheduler.hpp` makes the internal thread-residency assertion private.

**Symptom**
The scheduler class declared a retry helper and a discipline count used only in its implementation. The residency assertion was public even though only Scheduler and its nested Pi implementation called it.

**Cause**
The active-scheduler retry helper was a static member to call the CPU selector implicitly. The retry itself only needs a candidate CPU, the target scheduler's lock and Active state, and a callback that places the thread. The CPU selector needs Scheduler's access to the thread's private scheduling entity.

**Change**
Before:

```cpp
auto const target_cpu = Self::find_active_scheduler_for(to_migrate, place);
```

After:

```cpp
auto const target_cpu = find_active_scheduler_for<select_target_cpu>(to_migrate, place);
```

The retry helper is in the anonymous namespace. Binding the private selector as a template argument keeps the selection policy at its existing access level. The helper still chooses a candidate, takes its run queue lock, checks Active under that lock, and calls the placement callback with the lock held. The discipline count is local to the loop that uses it.

**Reference**
Zircon selects a candidate in `Scheduler::FindTargetCpu()` in `zircon/kernel/kernel/scheduler.cc`. Its retry helper is `Scheduler::FindActiveSchedulerForThread()` in `zircon/kernel/include/kernel/scheduler.h`.

**Verification**
Changed-line clang-format passed and clang-tidy produced no diagnostics on the changed lines. The Fair configuration passed syntax checks for `scheduler.cpp` and `scheduler_pi.cpp`. QEMU dbg passed 46 shell checks with 5 skipped. QEMU rel passed 44 shell checks with 8 skipped and reached all 7 required markers. All 183 host tests and the four-CPU Hotplug, Steal and WaitQueue kernel tests passed.

**Deliberately not done**
The CPU selector and reschedule helper retain the private-member access needed to read a thread's scheduling entity and verify its canary, respectively.

**How it was found**
A call-site search isolated the retry helper's users and the two users of the residency assertion. A syntax probe of file-local CPU selection identified the private access to `Thread::scheduling_entity()`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Which state is rechecked after taking the run queue lock?
- Which access privilege belongs to the CPU selector?
- Why can the nested Pi implementation still call the residency assertion?

## Name the thread placement callback helper with_target_scheduler (2026-11-07)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` renames the local helper and updates its callers and comments.

**Symptom**
`find_active_scheduler_for()` reads like a lookup, while its callers supply the work to perform on the selected scheduler.

**Cause**
The helper uses the CPU selector to pick a candidate. It takes that scheduler's run queue lock, retries if the scheduler is inactive, and invokes the callback while the lock is held. The name described only the search.

**Change**
Before:

```cpp
auto const target_cpu = find_active_scheduler_for<select_target_cpu>(to_migrate, place);
```

After:

```cpp
auto const target_cpu = with_target_scheduler<select_target_cpu>(to_migrate, place);
```

`target` identifies the scheduler chosen for the thread. `with` signals that the helper runs the callback within the lock scope. The helper returns the chosen CPU after the callback runs. The unpark comment now refers to its actual CPU selection path.

**Reference**
Zircon's corresponding helper is `Scheduler::FindActiveSchedulerForThread()` in `zircon/kernel/include/kernel/scheduler.h`. This project's spelling puts the callback contract in the name.

**Verification**
Changed-line clang-format passed and clang-tidy produced no diagnostics on the changed lines. QEMU dbg passed 46 shell checks with 5 skipped. QEMU rel passed 44 shell checks with 8 skipped and reached all 7 required markers. All 183 host tests passed.

**Deliberately not done**
None

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Which function picks the candidate CPU?
- What lock does the callback run under?
- What value does the helper return?
