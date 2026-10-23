# kernel-task changes

## A thread suspended or killed before it ever ran is started, so it can act on the signal (2026-10-23)

**Files**
- `kernel/kernel/task/thread.cpp` — `Thread::suspend_or_kill()` has a State::Initial case
- `kernel/commands/sleep_test.cpp` — a thread suspended, and one killed, before its first run
- `docs/repairs/261023/kernel-task.md` — the item removed

**Symptom**
Latent, about to become reachable: `.agents/tasks/task-control/` step 7 starts a thread suspended (`ThreadDispatcher::activate()` with `suspend`), which calls `task::Thread::suspend()` on a kernel thread that is still Initial. Before this change that logged "Unexpected state Initial in suspend_or_kill" and left the thread Initial for good: never suspended in the sense anyone could observe, and, for a kill, never dead.

**Cause**
The switch over the target's state decides how to bring the thread to the one place a signal is acted on, `process_pending_signals()` on its way back to ring 3: interrupt it if it runs elsewhere, wake it if it waits interruptibly, leave it if it is Ready. A thread that has never run is in none of those: it has no wait to end and no CPU to interrupt. The only way to get it to that place is to start it, and nothing did, so the signal was set on a thread that would never look at it.

**Change**
A State::Initial case that does what `resume()` does for Initial:

```cpp
case State::Initial: {
    Scheduler::unpark_thread(this, UnparkOptions::Initial);
    break;
}
```

A user thread then reaches `ThreadDispatcher::trampoline()`, whose `process_pending_signals()` runs before the first user instruction, and stops there or dies there. A kernel thread has no such point and meets the signal in its first interruptible wait, which is what the test watches. Starting the thread is safe here because the one caller that can reach an Initial thread, `activate()`, does so after saving the entry and binding the thread (`docs/changes/261023/object.md`).

**Reference**
Zircon `Thread::SuspendOrKillInternal()`, the THREAD_INITIAL case (`kernel/kernel/thread.cc:624-652`): `Scheduler::Unblock(this)`, so the thread "will see the signal and go to SUSPEND before running user code". Its own TODO notes that the older kill code thought waking an Initial thread unsafe because the thread might still be under construction; the ordering in `activate()` is what answers that here.

**Verification**
- `sleep.test`: 16 checks pass, among them a thread suspended before it ran (its interruptible sleep answers InternalIrqRetry) and one killed before it ran (InternalIrqKilled). With `thread.cpp` reverted, both log "Unexpected state Initial in suspend_or_kill" and fail: the thread never runs.
- `threadjoin.test`, `waitqueue.test` pass.
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
An assertion that nothing else suspends or kills an Initial thread while its creator is still setting it up: the only such caller is `activate()`, and a kernel thread's creator holds the only pointer to it until it resumes it.

**How it was found**
The review of the task-control design; recorded in `docs/repairs/261023/kernel-task.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. For each state a target can be in, how does `suspend_or_kill()` get it to `process_pending_signals()`, and why is Initial the one that needs the thread started?
2. Why can a kernel thread's test not watch for the thread to stop, and what does it watch instead?
3. What makes starting an Initial thread from `suspend_or_kill()` safe here, when Zircon's own comment doubts it?
