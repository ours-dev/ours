# kernel-task changes

## Title: kernel/task answers for every Status it used to drop (system-abi-surface step 2, part 2) (2026-10-03)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp`, `scheduler_pi.cpp` — the enqueues and dequeues assert Ok in a debug build; the header's TODO about a delayed dequeue now says why it cannot happen there; `unpark_preempt()`'s Unimplemented is dropped explicitly, with what it costs.
- `kernel/kernel/task/sched/sched_clock.cpp` — programming the preemption tick asserts Ok.
- `kernel/kernel/task/cpu_stopper.cpp`, `dpc.cpp` — the waits and the `detach()` of a fresh thread assert Ok; `resume()` is dropped explicitly.
- `kernel/kernel/task/thread.cpp` — the user-thread hooks, `kill()` and `set_name()` dropped explicitly.

**Symptom**
Sixteen of the 44 silent drops counted for step 2 are in kernel/task. None was wrong today, but two kinds of them rest on an assumption nothing checked, and one hides an unimplemented feature.

**Cause**
Two kinds of drop, which want different answers:
- The callee only ever answers Ok: `Thread::resume()`, `suspend_or_kill()` behind `kill()`, `ThreadDispatcher`'s `on_suspend()`/`on_resume()`/`on_exit()`, `Name::set()` but for a null name. Dropping these is safe as long as the callee stays so. They are dropped with `static_cast<void>` and the reason.
- The callee can fail, but not in this caller's state:
  - `Scheduler::dequeue_thread()` answers `Status::Delayed` when Fair delays the dequeue of an ineligible sleeper. That needs `DequeueOptions::Sleep`, which neither `deactivate_thread()` (a migration) nor the PI re-sort passes.
  - `detach()` fails only on a thread already Dead, where it must reap through `join()`, which is not supported. A thread just spawned is not Dead.
  - `sync::Event::wait()` with no deadline, not interruptible, answers whatever `signal()` was given, and the CPU stopper gives only Ok.
  - The one-shot tick device takes any deadline, forcing a late one.

  These assumptions are now checked: `DEBUG_ASSERT(Status::Ok == status)`, so the day a schedule that refuses an enqueue appears, or a migration passes Sleep, a debug build stops at the line instead of corrupting the run queue's counts.

The third: `Fair::unpark_preempt()` answers `Unimplemented`. A thread woken into the running thread's own schedule never preempts it, and waits for its tick. The drop is explicit and the comment says so. Implementing it is fair.cpp's business, which this session does not touch, and `docs/repairs/260917/kernel-task-sched.md` item 2 already concerns that function.

**Change**

```cpp
// Scheduler::deactivate_thread(), before
dequeue_thread(thread, options);
// after
// Without DequeueOptions::Sleep no schedule delays a dequeue (Fair delays
// only a sleeper's, try_to_delay_dequeue()), so the thread is off the
// queue when this returns, as Migrating promises.
[[maybe_unused]] auto const status = dequeue_thread(thread, options);
DEBUG_ASSERT(Status::Ok == status, "a dequeue for a migration was delayed");
```

**Reference**
Linux's `dequeue_task()` (kernel/sched/core.c) returns a bool, false when the fair class delays the dequeue of a sleeper (kernel/sched/fair.c), so its callers can tell; Zircon's scheduler has no delayed dequeue. The assert here records that the callers that cannot act on it are never given one.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); no new assertion fired.
- `mutex.stress` on dbg, through `scripts/kshell_run.py`: `ok -- 20000 rounds x 3 threads`, no error line. It puts the sleep, wakeup, PI and migration paths under load with the assertions compiled in.

**Deliberately not done**
- `Fair::unpark_preempt()` (above), and the delayed dequeue's own TODO in `reschedule_common()`, which is about the Sleep path and not a drop.
- The remaining 18 drops (top, kshell, object, arch, driver, ktl), and the attribute itself, in the next commits.

**How it was found**
The count for step 2.

**The user's prediction**
None for this part; see the mm entry of the same day.

**Three questions you should be able to answer**
- When is a `static_cast<void>` the right answer to a dropped Status, and when a `DEBUG_ASSERT`?
- Why can `deactivate_thread()` assume its dequeue is not delayed, and what would break if it were?
- What does a woken thread wait for today when it should preempt the running one?
