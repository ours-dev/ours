# kernel-task outstanding repairs

## 1. A thread suspended or killed before it first ran is only logged

**Severity**
Medium: unreachable today, because nothing suspends or kills a thread in State::Initial (`ThreadDispatcher::activate()` is always called with false), and reachable as soon as a thread can be started suspended.

**Symptom**
Latent, cannot be triggered today. `Thread::suspend_or_kill()` on a thread in State::Initial falls into `default:` and logs "Unexpected state" (`kernel/kernel/task/thread.cpp:355-357`); the thread's signal is set but nothing makes it run to where the signal is acted on.

**Cause**
The switch over the target's state has no Initial case. A thread that has never run has no wait to be woken from and no CPU to interrupt; it has to be started, so that the first thing it does is meet the signal at its first return towards ring 3.

**Change**
An Initial case that unparks the thread (`Scheduler::unpark_thread(this, UnparkOptions::Initial)`), as `resume()` does for Initial. Its test needs a kernel thread that observes the signal itself, for example through an interruptible sleep that must answer InternalIrqRetry or InternalIrqKilled, because a kernel thread never runs `process_pending_signals()`.

**Reference**
Zircon `Thread::SuspendOrKillInternal()`, the THREAD_INITIAL case (`kernel/kernel/thread.cc:624-652`), which unblocks the thread so it meets the signal before any user code; its own TODO records that the older kill code thought waking an Initial thread unsafe.

**Acceptance**
A kshell case: suspend and kill a thread before it first runs; it runs, its interruptible sleep answers the signal, and nothing is logged.

## 2. A wait that masks Suspend still ends on it when another signal is pending

**Severity**
Low: needs SampleStack pending inside the kernel, and SampleStack is set only by a tick that interrupted ring 3 and is consumed at that interrupt's tail.

**Symptom**
Latent, cannot be triggered today. `WaitQueue::park_thread_preamble()` decides to stop with `signals & ~sigmask`, but then answers with `check_kill_or_suspend_signal()`, which ignores the mask (`kernel/kernel/task/wait_queue.cpp:59-64`, `kernel/include/ours/task/thread.hpp:455-462`). With Suspend masked and both Suspend and SampleStack pending, a masked wait answers InternalIrqRetry, and `ExceptionDispatcher::wait_for_handle_close()` would loop on it without sleeping.

**Cause**
The decision and the answer read the signals through different filters.

**Change**
Answer from the unmasked signals only: Kill if it is pending and not masked, Suspend likewise, and wait otherwise. This departs from Zircon, which has the same shape, so it is the user's decision; until a signal bit can be pending inside the kernel it changes nothing.

**Reference**
Zircon `WaitQueue::BlockEtcPreamble()` (`kernel/include/kernel/wait_queue_internal.h:61-66`) decides with `signals() & ~signal_mask` and answers with `CheckKillOrSuspendSignal()`, exactly as here.

**Acceptance**
A kshell case: a wait with Suspend masked and both bits pending parks instead of answering.
