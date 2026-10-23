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

## A ring-3 page fault whose wait a suspend or a kill ended is not a fatal fault (2026-10-23)

**Files**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::page_fault()` answers Ok for a ring-3 fault that ended with InternalIrqRetry or InternalIrqKilled, and asserts that a ring-0 fault never ends with InternalIrqRetry
- `kernel/mm/include/ours/mm/fault.hpp` — removed: it declared `mm::handle_vm_fault`, which nothing defined or called
- `kernel/mm/include/ours/mm/vm_mapping.hpp`, `kernel/arch/x86/mm/fault.cpp` — include `vm_fault.hpp`, which they had through `fault.hpp`

**Symptom**
Latent until a program can be suspended while it waits on a page source: a ring-3 fault on a pager VMO that a suspend interrupted came back from `page_fault()` as InternalIrqRetry, which the x86 handler treats as unresolvable, so the thread raised a fatal page fault and, with no handler, its process was killed. A suspend would have ended a process.

**Cause**
A fault is the other place, besides a blocking system call, where a thread waits in the kernel on behalf of ring 3, and the same statuses end its wait: InternalIrqRetry to let the thread stop, InternalIrqKilled to let it die. Both mean "the fault is not over, but the thread has to go elsewhere first", not "the address cannot be resolved". The fault path only knew Ok and failure.

**Change**
In `Thread::Current::page_fault()`, the arch-neutral entry the x86 handler calls:

```cpp
if (!(reason & mm::VmfCause::User)) {
    DEBUG_ASSERT(Status::InternalIrqRetry != status, ...);
    return status;
}
if (Status::InternalIrqRetry == status || Status::InternalIrqKilled == status) {
    return Status::Ok;
}
```

Ok returns from the handler; the exception's tail runs `process_pending_signals()`, where the thread stops or dies; a resumed thread runs the faulting instruction again, which faults again and asks for the page again. A ring-0 fault -- a user copy -- waits without letting a suspend end it, so the assertion holds; translating it would send the copy straight back into a wait that ends at once while the signal is pending, the busy loop Zircon's own TODO describes. A kill may end a ring-0 fault's wait, and is passed on: the copy fails and the thread dies at the end of its system call.

The translation was to go in `mm::handle_vm_fault`, declared in `kernel/mm/include/ours/mm/fault.hpp` for an arch-neutral fault entry and never defined. The user had the declaration removed: `Thread::Current::page_fault()` already is that entry.

**Reference**
Zircon `TranslatePageFaultStatus()` in `vmm_page_fault_handler()` (`kernel/vm/vmm.cc:48-79`): INTR_RETRY becomes OK for every fault, with a TODO about the busy loop for kernel-mode ones, and INTR_KILLED becomes OK for user-mode ones.

**Verification**
- `pagesource.test` (43), `usercopy.test` (18), `vmosnapshot.test` (205) and `vmaseal.test` (13) pass: every fault they take is a ring-0 fault on a user address and goes through the new assertion.
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not exercised: the ring-3 translation needs a thread that waits on a page source in a ring-3 fault and is suspended, which needs `task_suspend` (task-control step 7) and a user pager.

**Deliberately not done**
Zircon's translation of INTR_RETRY for a kernel-mode fault: here such a wait cannot be ended by a suspend, and the assertion says so instead of hiding a loop.

**How it was found**
The task-control design: the second consumer of InternalIrqRetry, after the vDSO retry.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. What does the thread do between the handler answering Ok and the faulting instruction running again?
2. Why is a ring-0 fault never ended by a suspend, and what would translating its retry to Ok do?
3. Why is a kill that ends a ring-0 fault's wait passed on rather than turned into Ok?
