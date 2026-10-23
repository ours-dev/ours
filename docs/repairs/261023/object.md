# object outstanding repairs

## 1. A process that handles its own exceptions waits for itself for ever

**Severity**
Low: it hurts only the process that does it, and it is Zircon's rule too; what makes it permanent here is that nothing can kill the process.

**Symptom**
A single-threaded process binds its own exception channel (`task_create_exception_channel` on its own process or thread handle, which every launched process holds) and then faults: its only thread waits in `ExceptionDispatcher::wait_for_handle_close()` for the handle to the exception to be closed, and that handle sits in a channel only that thread could read. The process never reaches Dead, so its memory and handles are never reclaimed, and anyone waiting for its Terminated waits for ever. Reachable today.

**Cause**
A thread cannot handle its own exception, by design. Zircon lets another process recover with `zx_task_kill`, which wakes the wait with INTR_KILLED. This kernel has no `task_kill` (also noted in `docs/repairs/261004/init.md`), and no process can start a second thread to read the channel. Found by the self-deadlock audit of 2026-10-23; no kernel lock is held across the wait.

**Change**
`task_kill` (`.agents/tasks/task-control/PLAN.md` step 8) lets the process's parent end it; the wait is already interruptible by Kill.

**Reference**
Zircon `ExceptionDispatcher::WaitForHandleClose()` and `zx_task_kill`.

**Acceptance**
A shell check: a child binds its own process's exception channel and faults; the shell kills it with `task_kill` and sees Terminated.
