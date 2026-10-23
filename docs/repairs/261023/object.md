# object outstanding repairs

## 1. A second `process_start` on a thread that has not run yet clears its entry, and the kernel stops

**Severity**
High: a user program can stop the kernel with two calls it is allowed to make.

**Symptom**
`process_start(process, thread, ...)` twice on the same thread, the second before the thread has run its first instruction: the second call fails with BadState, and the thread then reads an empty entry in `ThreadDispatcher::trampoline()` -- `DEBUG_ASSERT(self->m_user_entry)` in the dbg image, a read of an empty `ustl::Option` in the rel image. The new thread is usually still Ready when the caller makes its second call, so the window is the common case, not a race.

**Cause**
`ThreadDispatcher::start()` (`kernel/object/thread_dispatcher.cpp:165-191`) writes `m_user_entry` and `m_starts_process` first, with no lock and before anything has checked that the thread is still Initial, and clears `m_user_entry` when `attach_thread()` fails. The state check comes later, in `ProcessDispatcher::attach_thread()` (process state) and `create_kernel_thread()` (thread state), so a call that is going to be refused has already overwritten, and then erased, the entry of a thread that was started by an earlier call and is about to read it. `m_user_entry` has no `TA_GUARDED`, so the analysis does not see it.

**Change**
Save the entry only under the thread's lock, after the check that the thread is Initial, and never clear an entry once the thread has a kernel thread: move the two writes into `create_kernel_thread()` (which already checks Initial under the lock) or `activate()`, and mark the fields `TA_GUARDED`. A refused start then touches nothing. A `thread_start` system call (`.agents/tasks/task-control/`) would make this a one-line user action, so it lands before that.

**Reference**
Zircon `ThreadDispatcher::MakeRunnable()` (`kernel/object/thread_dispatcher.cc:164-176`): the lifecycle is checked under the thread lock before `user_entry_` is saved.

**Acceptance**
A shell check: `process_start` twice on the same thread, the second at once; the second answers BadState and the child runs normally. Both images boot.

## 2. A process that handles its own exceptions waits for itself for ever

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
