# kernel-task outstanding repairs

## 1. A wait that masks Suspend still ends on it when another signal is pending

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

## 2. Priority inheritance takes a wait queue's lock it already holds

**Severity**
Low: unreachable today, because nothing instantiates `Scheduler::Pi::common()`; a certain self-deadlock the day priority inheritance is wired up.

**Symptom**
Latent, cannot be triggered today. `Scheduler::Pi::common()` asserts that the wait queue's lock is held (`kernel/kernel/task/sched/scheduler_pi.cpp:69`) and then calls `WaitQueue::update_parked_thread_effective_profile()`, which takes that same spin lock (`kernel/kernel/task/wait_queue.cpp:156`). In the dbg image that is `SpinLock::acquire()`'s `!is_held()` assertion; in the rel image the CPU spins for good with interrupts off.

**Cause**
The update was written as a public entry point that locks for itself, and the caller was written for a `_locked` variant that does not exist. Found by the self-deadlock audit of 2026-10-23, which found no other lock taken twice in the signal, scheduler, wait-queue, interrupt, object and page-source paths.

**Change**
Split `update_parked_thread_effective_profile()` into a `_locked` body, which asserts the lock, and the existing entry point, which takes it and calls the body; `Pi::common()` calls the `_locked` one.

**Reference**
Zircon `WaitQueueBase::UpdateBlockedThreadEffectiveProfile()` (`kernel/include/kernel/wait.h:260`, `kernel/kernel/wait.cc:616`), declared `TA_REQ(get_lock(), t)`: it requires the queue's lock and takes nothing.

**Acceptance**
Builds with `Pi::common()` instantiated by a test, and the test boosts a parked thread's priority without the assertion firing.
