# syscall changes

## A program can start a later thread of its process, and a thread can end itself (2026-10-23)

**Files**
- `system/public/ours/syscalls.gni` — `thread_start` and `thread_exit`; `thread_create`'s doc no longer says a thread of a running process cannot be started
- `kernel/lib/syscall/src/task.cpp`, `kernel/lib/syscall/BUILD.gn` — the two handlers, in place of their placeholders
- `kernel/object/thread_dispatcher.cpp`, `kernel/object/include/ours/object/thread_dispatcher.hpp` — a thread that exits by its own hand passes Dying (`on_exit()`)
- `system/app/shell/shell_tests.cpp` — `check_thread_start()`: a second thread of the shell, on a stack it maps, reporting back on a channel
- `.agents/tasks/user-pager/PLAN.md` — its step 8 is this

**Symptom**
None: a missing interface. A process had exactly one thread: `thread_create` made threads that only `process_start` could start, and only as a process's first, and a thread could end only with its process. Every later step of `.agents/tasks/task-control/` needs a second thread, to be the one suspended while another watches.

**Cause**
`thread_start` and `thread_exit` were placeholders with no `syscalls.gni` entry, waiting for their preconditions (the design's P1 row: a TLB shootdown and the VMO lock on the fault path), which are met.

**Change**
`thread_start(thread, entry, stack, arg1, arg2)` resolves the thread with ManageThread and calls `ThreadDispatcher::start(..., false)`, which goes through `ProcessDispatcher::attach_thread()` with the process required to be Running; `thread_exit()` is `ThreadDispatcher::exit_current()`, and is `noreturn`. The thread's entry gets `arg1` in %rdi and `arg2` in %rsi; it has no return address, so its code must end in `thread_exit`.

With them, the user's decision D7: a thread that leaves by its own hand passes Dying, as a killed one already did. `on_exit()`, which every exiting user thread runs, makes a Running thread Dying before anything else:

```cpp
{
    Guard guard{get_lock()};
    if (m_state == State::Running) {
        m_state = State::Dying;
    }
}
```

From there the thread will not run in ring 3 again, so a suspend asked of it (task-control step 7) must be refused rather than handed a token whose stop never comes.

**Reference**
Zircon `sys_thread_start_regs()` (`kernel/lib/syscalls/thread.rs`: `ZX_RIGHT_MANAGE_THREAD`, then `thread.start(..., false)`), `sys_thread_exit()` and `ThreadDispatcher::ExitingCurrent()` (`kernel/object/thread_dispatcher.cc:350-359`), which sets DYING before the thread goes. Zircon's `zx_thread_start` doc names ZX_RIGHT_WRITE, but its code checks MANAGE_THREAD; the code is what this follows. Zircon's raw call also takes a thread pointer and an ABI register (`thread_start_regs`); ours takes the five arguments its program-facing call has, and a thread pointer can come behind the same call when thread-local storage needs it (`.agents/tasks/task-control/DESIGN.md`, open question).

**Verification**
- Shell, both images (`thread_start checks passed`): BadHandle, BadType for a channel, AccessDenied without ManageThread, InvArgs for an entry outside user space; the start, after which the second thread reports both its arguments on the channel and its handle reads Terminated and not ThreadRunning; a second start at once and a start of the ended thread are BadState.
- The second start at once is the case `docs/changes/261023/object.md` could not test: the thread has most likely not run when it is made, and before that fix it would have cleared the thread's entry.
- `ours-verify`: dbg PASS (40 passed, 4 skipped), rel PASS (38 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not observable yet: the Dying a voluntary exit passes; a suspend refused for it is checked at step 7.

**Deliberately not done**
A thread pointer argument and the `thread_start_regs` shape; `thread_read_state` and `thread_write_state`.

**How it was found**
None: step 6 of `.agents/tasks/task-control/PLAN.md`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why must a thread started with `thread_start` end in `thread_exit` instead of returning from its entry?
2. What decides whether `thread_start` or `process_start` starts a given thread, and where in the kernel is that checked?
3. Why does a thread exiting by its own hand pass Dying, and what would go wrong with a suspend if it went straight from Running to Dead?

## A program can kill a process, and a process that waits for itself can be ended (2026-10-23)

**Files**
- `system/public/ours/syscalls.gni` — `task_kill`
- `kernel/lib/syscall/src/task.cpp` — its handler
- `docs/design/260923/userspace-interface.md` — the `task_kill` row revised: a process only (D2)
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — two probes: `own_exception_channel`, which waits for itself, and `kill_self`
- `system/app/shell/shell_tests.cpp` — `check_task_kill()` on both images, `check_task_kill_probes()` on dbg_1
- `docs/repairs/261023/object.md` — removed: its one item is fixed by this
- `docs/repairs/261004/init.md` — item 2 notes that `task_kill` exists now, but not for a job

**Symptom**
A process could end only from inside, by `process_exit` or by a fault nobody handled. One that did neither could not be ended at all, and `docs/repairs/261023/object.md` item 1 was such a process, reachable today: a single-threaded process that binds its own exception channel and then faults waits in `ExceptionDispatcher::wait_for_handle_close()` for an answer only it could give. Its memory and handles were never reclaimed, and its Terminated never came.

**Cause**
`task_kill` was a row in the design table with no entry in `syscalls.gni` and no handler. Everything under it was there already, which is why the change is small: `ProcessDispatcher::kill()`, through which a fault nobody handles already ends a process, and a kill that reaches a thread wherever it is. The objects:

- `ProcessDispatcher`: its state (Initial, Running, Dying, Dead), its return code, and the list of its started threads.
- `ThreadDispatcher`: its state (Initial, Running, Suspended, Dying, Dead) and its kernel thread.
- `task::Thread`: its signal bits (Kill, Suspend) and its scheduling state (Running, Ready, Blocked, Suspend).

The events of a kill, in order:

1. `ProcessDispatcher::kill()` takes the process lock. A process Dead or Dying is left alone, and keeps the code it is dying with. Otherwise the code is recorded; a process with no thread on its list (never started) is Dead at once, and `finish_dead_transition()` runs outside the lock; any other becomes Dying, and every thread on the list gets `ThreadDispatcher::kill()`.
2. `ThreadDispatcher::kill()` makes a Running or Suspended thread Dying and calls `task::Thread::kill()`.
3. `suspend_or_kill()` sets the Kill bit under the thread's PI lock, then acts by where the thread is: running on another CPU, an IPI, so that it enters the kernel; blocked in an interruptible wait (a system call, an exception's wait for its answer), unparked with InternalIrqKilled; stopped by a suspend, unparked; running on this CPU, nothing, since that is the caller killing itself.
4. On its way back to ring 3 every thread runs `process_pending_signals()`, which sees Kill and exits. A suspended thread sees it in `do_suspend()` the moment it wakes, and exits without running `on_resume()`.
5. Each exiting thread leaves the process (`detach_thread()`) and raises its own Terminated. The last one makes the process Dead and runs `finish_dead_transition()`, which raises the process's Terminated last.

The process waiting for itself is step 3's second case: the exception's wait is interruptible, so a kill ends it, which is what the design relied on when it filed the item.

**Change**
The handler resolves the handle without a type, tells the tasks apart, and only then looks at the right:

```cpp
auto task_kill(ours::Uandle const task) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::Dispatcher> object;
    auto rights = Rights::None;
    if (auto const status = table.get_untyped(task, &object, &rights); status != Status::Ok) {
        return status;
    }

    auto process = object.downcast<object::ProcessDispatcher>();
    auto const thread_or_job =
        object.downcast<object::ThreadDispatcher>() || object.downcast<object::JobDispatcher>();
    if (!process && !thread_or_job) {
        return Status::BadType;
    }
    if ((rights & Rights::Destroy) != Rights::Destroy) {
        return Status::AccessDenied;
    }
    if (!process) {
        return Status::NotSupported;
    }

    process->kill(TASK_RETCODE_SYSCALL_KILL);
    return Status::Ok;
}
```

The order is this project's precedence of errors (design 4.5: BadHandle, then BadType, then AccessDenied), so a VMO handle without Destroy is BadType. A thread and a job answer NotSupported (D2, the user's call). A thread killed from outside dies wherever it is, holding a lock of its program's perhaps, and the rest of its process goes on with that lock held for good; a thread ends by `thread_exit` or with its process. A job has no life cycle in this kernel yet: no `job_create`, and nothing that makes a job Dead.

The call's documentation says a process that kills itself does not return from it. The handler does return Ok, but into the kernel's way back to ring 3, where the calling thread meets its own Kill bit and exits, so no program ever sees that Ok.

Two probes serve the tests: `own_exception_channel` binds its own process's exception channel, answers "bound" and faults, which is the repair item's process; `kill_self` kills its own process and answers "survived" if the call returns.

**Reference**
Zircon `sys_task_kill()` (`kernel/lib/syscalls/task.rs`): `get_with_rights(handle, ZX_RIGHT_DESTROY)` first, then a job is killed with `JobDispatcher::kill()`, a process with `ProcessDispatcher::kill()`, a thread is NOT_SUPPORTED and anything else WRONG_TYPE. Two differences: the right is checked after the type here, so a handle to a non-task without Destroy is ACCESS_DENIED in Zircon and BadType here; and a job is NotSupported here. Zircon `ProcessDispatcher::Kill()` (`kernel/object/process_dispatcher.cc:339`) is what `ProcessDispatcher::kill()` follows: Dead returns, the code is recorded once, no thread means Dead at once with `FinishDeadTransition()` outside the lock, otherwise Dying, whose state change kills every thread. The documentation of `zx_task_kill` (`zircon/vdso/task.fidl`) says killing a thread is not supported and a process killing itself does not return from the call.

**Verification**
- Shell, both images (`task_kill checks passed`): BadHandle; BadType for a VMA; NotSupported for a thread; NotSupported for the job (AccessDenied where the shell's handle lacks Destroy; on both images it has it); AccessDenied for a process handle without Destroy. A process never started, killed, reads Terminated as the call returns (polled), with Exited and TASK_RETCODE_SYSCALL_KILL (-1024) in `object_get_info`; a second kill of it answers Ok.
- Shell, dbg_1 (`task_kill probe checks passed`), each probe ending with -1024: one blocked in its runtime's wait for processargs; one whose only thread is suspended, the thread taken from a job debugger channel's ProcessStarting with `exception_get_thread`, suspended until it reads ThreadSuspended, then killed, after which it reads Terminated and its token closes with Ok; the process waiting for itself, which is the repair item's acceptance; and `kill_self`, which never answers.
- The dbg log shows each kill landing where it was aimed: the suspended probe's thread went quiet in its wait, the suspend's IPI stopped it and the kill's IPI woke it to die; the self-waiting probe's last fault was followed by silence on its CPU until the kill's IPI; the self-kill took no IPI.
- Control: with `ThreadDispatcher::kill()` temporarily leaving a Suspended thread alone, the dbg boot hung right after `task_kill(a process whose thread is suspended) = Ok`, and ours-verify failed with the console silent; restored, it passes.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- Killing a job (D2): nothing makes or ends a job yet. `docs/repairs/261004/init.md` item 2 needs it to stop a component, and now says so.
- Killing a thread (D2, as Zircon).
- On the dev image, killing a process that runs: the dev shell cannot launch a program, so its checks kill a process never started.
- A thread-state query: that the suspended probe stopped in its wait, rather than on its way there, is read from the log, not checked.

**How it was found**
None: step 8 of `.agents/tasks/task-control/PLAN.md`. The process that waits for itself was found by the self-deadlock audit of 2026-10-23.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. A process whose only thread is suspended is killed: what wakes the thread, and why does it not run `on_resume()` before it dies?
2. Why does a process that kills itself never see the Ok its handler returns, and why does that kill send no IPI?
3. Why does Zircon refuse to kill a single thread, and what could a program observe if it could?
