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
