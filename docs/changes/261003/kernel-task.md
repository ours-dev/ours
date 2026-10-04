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

## Title: WaitQueue::unpark_thread() no longer takes the woken thread's PI lock a second time (2026-10-03)

**Files**
- `kernel/kernel/task/wait_queue.cpp` — `unpark_thread()` asserts the PI lock its callers hold and ends in the new `wake_dequeued_thread_locked()`; `wake_dequeued_thread()` is now the guard around that body.
- `kernel/include/ours/task/wait_queue.hpp` — states `unpark_thread()`'s contract (caller holds the thread's PI lock, as Zircon's `UnblockThread()` requires) and declares `wake_dequeued_thread_locked()`.
- `kernel/commands/wait_queue_test.cpp` — new `waitqueue.test`: a timed wait nobody signals, and a suspend aimed at a thread parked interruptibly.
- `kernel/commands/BUILD.gn` — builds it.

**Symptom**
Latent: no path a normal boot runs reaches it. Any wait that ends by its own deadline, and any suspend or kill of a thread parked interruptibly, stops dbg_1 with `Assertion failed on !is_held()`; a build without assertions spins for good, interrupts off, on a lock its own CPU holds. `waitqueue.test` on the code before this change panicked that way in both of its cases (the timeout case about 9.5 ms in, the suspend case about 50 ms in, after its polling). It stayed unseen because `object_wait_one` refuses finite deadlines and no system call suspends or kills another thread yet; the first `task_kill`, the first multi-threaded process to be killed, and the exception channel's interruptible wait would each have hit it.

**Cause**
A wait queue wakes a thread in two different shapes, and they disagree about who holds the thread's PI lock.

- The *queue-driven* wake (`notify_one()`, `notify_all()`, `OwnedWaitQueue::notify_threads()`) starts from the queue: it takes the queue lock, picks the head waiter, drops the queue lock, and only then takes the woken thread's PI lock, because the PI lock is the outer one (PI lock, then queue lock). `wake_dequeued_thread()` was written for this shape and takes the PI lock itself.
- The *thread-directed* wake starts from a thread. `Thread::suspend_or_kill()` must read the target's state and its waiter's interruptible flag under the target's PI lock before deciding to wake it, and `WaitQueue::handle_timeout()` must win that same lock with `trylock_or_cancel()` before it may touch the waiter (the thread cancelling its own timer holds the lock and spins on the handler). Both arrive at `WaitQueue::unpark_thread()` already holding the PI lock.

`unpark_thread()` dequeued the waiter and then called `wake_dequeued_thread()`, which took the PI lock again. `sync::SpinLock` is not recursive: a second acquire on the same CPU waits for a release that only this CPU could perform. The bug is not "one lock too many" in the abstract. Holding the lock across the decision is what makes the decision valid: if `suspend_or_kill()` dropped it before waking, the thread could be woken, run, and park somewhere else in between, and the wake would hit the wrong wait. So the fix is not to release early. The callee has to be told that the lock is already held.

**Change**
The wake body is split from the lock that guards it, and the thread-directed path calls the body directly:

```cpp
// before
auto WaitQueue::unpark_thread(Thread *thread, Status status) -> void {
    DEBUG_ASSERT(!!(thread->state() & Thread::State::Parked));
    if (!dequeue_waiter(thread->waiter_state(), status)) {
        return;
    }
    wake_dequeued_thread(thread, status);   // takes thread's PI lock again
}

// after
auto WaitQueue::unpark_thread(Thread *thread, Status status) -> void {
    thread->get_pi_lock().assert_held();
    DEBUG_ASSERT(!!(thread->state() & Thread::State::Parked));
    if (!dequeue_waiter(thread->waiter_state(), status)) {
        return;
    }
    wake_dequeued_thread_locked(thread, status);
}

auto WaitQueue::wake_dequeued_thread(Thread *thread, Status status) -> void {
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> guard{thread->get_pi_lock()};
    wake_dequeued_thread_locked(thread, status);
}
```

The `assert_held()` turns the contract into a run-time check in dbg and gives the thread-safety analysis the capability for the code below it, as `WaiterState::sleep()` does (a `TA_REQUIRES` cannot name the lock here, because `wait_queue.hpp` sees `Thread` only as a forward declaration). The locked body still needs what the guard gave the old one, interrupts and preemption off; both callers provide it (`suspend_or_kill()` holds the lock `IrqSaveAndPreemptDisable`, `handle_timeout()` runs in the timer interrupt).

Rejected: releasing the PI lock in `suspend_or_kill()` before the wake (it invalidates the decision, see Cause); making the PI lock recursive (it hides exactly this kind of confusion about who owns a critical section, and Zircon's locks are not recursive either).

**Reference**
Zircon `WaitQueueBase::UnblockThread()` (kernel/kernel/wait.cc) is called with the thread's lock held, and its two thread-directed callers are the counterparts of these: `Thread::SuspendOrKillInternal()` (kernel/kernel/thread.cc) and `WaitQueue::TimeoutHandler()` (kernel/kernel/wait.cc). Linux has the same split inside one function: `try_to_wake_up()` takes `p->pi_lock` once, and the helpers that do the wake under it (`ttwu_queue()`, `ttwu_do_activate()`) run with it held and never take it again.

**Verification**
- On an isolated copy of HEAD with only this change (the shared worktree's build is broken by another session's unfinished shell work): `boot_check.py --out out/task-wake`, dbg PASS (20 passed), rel PASS (18 passed), 0 error lines.
- `kshell_run.py --out out/task-wake waitqueue.test` on dbg: `ok -- 3 checks`; the timed wait answered TimeOut after 12523 us, and the suspended waiter was woken.
- The same test on the code before this change: `Assertion failed on !is_held()` in each case, run separately.
- The test cannot run on rel: that image boots with `kernel.shell=false` since `ead5d12f`.
- clang-tidy: no finding on the changed lines.

**Deliberately not done**
- The suspended waiter still wakes with `Status::Ok`, and `suspend_or_kill()` still dereferences a `wait_queue()` that may be null; both stay in `docs/repairs/260924/kernel-task.md` item 2 and are the next two changes.
- `handle_timeout()` has the same possibly-null `wait_queue()` (`DEBUG_ASSERT(wq)`); it goes with the `suspend_or_kill()` one.
- Item 3 (the Sleeping branch) is a separate defect.

**How it was found**
The exception-channel exploration asked whether a thread blocked in the kernel can be killed. Reading `suspend_or_kill()`'s Parked branch against repair item 2 showed it still open; reading `wait_queue.cpp` for the fix found `handle_timeout()` doing the same thing, unrecorded. `waitqueue.test` was then written to reproduce both before fixing them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `suspend_or_kill()` keep the target's PI lock from its state check through the wake, instead of dropping it and calling the ordinary wake?
- Why does the queue-driven wake take the PI lock only after dropping the queue lock, and what would happen if it took it first?
- Why can `handle_timeout()` not simply acquire the PI lock, and what does `trylock_or_cancel()` give it instead?
