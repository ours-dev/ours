# task-control: a program can start, suspend, resume and kill its threads

**Date**: 2026-10-23
**Status**: Accepted

## Problem
The kernel can suspend, resume and kill a thread (`task::Thread::suspend()`, `resume()`, `kill()`, all on `suspend_or_kill()`), but a program cannot ask for any of it: `ThreadDispatcher::suspend()`/`resume()` only verify the canary, `thread_start`, `thread_suspend` and `thread_exit` are placeholders with no `syscalls.gni` entry, and there is no `task_suspend` or `task_kill`. A thread raises no object signal, so a program cannot even see that a thread stopped. The user asked for the interface of the whole thread-signal system.

The moment a program can suspend a thread, the internal status InternalIrqRetry reaches user space: every blocking call (`port_wait`, `object_wait_one`, `vmo_read` on a pager VMO) returns 31, and a page fault interrupted by a suspend becomes a FatalPageFault. Zircon consumes it in the vDSO and in the fault path; ours does neither.

## Goals and non-goals
Goals:

- `thread_start` and `thread_exit` (decision D1), `task_suspend` returning a suspend token, `task_kill` for a process (D2, D3).
- Thread object signals a program can wait on: Terminated, ThreadRunning, ThreadSuspended.
- InternalIrqRetry never reaches a program: the vDSO retries blocking calls (D4) and the fault path turns a user fault's retry into a re-executed instruction (D5). Both land before the first call that can suspend a thread.

Non-goals: suspending a process (D3), killing a job (no job lifecycle), killing a single thread from outside (D2), `task_suspend_token`, thread state read and write, thread-local storage, recording the wait mask on the thread (the sigmask question, option A kept).

## Design
Objects:

- Thread signal bits (`task::ThreadSignals::Suspend`, `Kill`): kernel only, one bit each, set under the thread's PI lock by `suspend_or_kill()` and acted on by `process_pending_signals()` on every way back to ring 3. Unchanged, except a State::Initial case (`docs/repairs/261023/kernel-task.md` item 1).
- `ThreadDispatcher` suspend count: how many tokens hold the thread, under the dispatcher's lock. Only the 0 to 1 step suspends the kernel thread and only 1 to 0 resumes it; a thread not yet started is only counted, and `activate()` reads the count under that same lock to start it suspended. A Suspended lifecycle state between Running and Dying (D6); `suspend()` answers BadState for Dying and Dead, `resume()` does nothing for them, and `on_suspend()` leaves a Dying thread alone, as Zircon's `Suspending()` does.
- Thread object signals (ABI): Terminated (bit 3, the task's), ThreadRunning (bit 4), ThreadSuspended (bit 5), switched with the lifecycle state. Terminated is raised outside the process lock, as the process raises its own.
- `SuspendTokenDispatcher` (`ObjectType::SuspendToken = 27`, rights Transfer|Inspect): holds the thread while a handle to it lives; `create()` suspends, `on_zero_handles()` takes the thread out under its lock and resumes it after letting go.
- vDSO retry: a stub generated for a call with `attrs = [ "blocking" ]` jumps back to the trap while the status is InternalIrqRetry. Possible in the stub because the kernel restores every argument register on the way out. The generator refuses `blocking` with `noreturn` or with a return that is not a Status.
- `task::Thread::Current::page_fault()`: the arch-neutral fault entry the x86 handler calls; for a fault from ring 3, InternalIrqRetry and InternalIrqKilled become Ok, so the fault is taken again after the resume, or the thread dies on its way out. (`mm::handle_vm_fault`, declared for this and defined nowhere, was removed at the user's word.)

Events, suspend and resume:

1. A program calls `task_suspend(thread, &token)`: Write on the thread; not the caller's own thread (NotSupported); not Dying or Dead (BadState).
2. The count goes 0 to 1 and `task::Thread::suspend()` sets the bit: an IPI if the thread runs elsewhere, InternalIrqRetry if it waits interruptibly.
3. On its way back to ring 3 the thread runs `process_pending_signals()`; `on_suspend()` makes it Suspended and raises ThreadSuspended; it parks.
4. The program's `object_wait_one(thread, ThreadSuspended)` returns.
5. The program closes the token: the count goes 1 to 0, the thread is unparked, `on_resume()` raises ThreadRunning, and it returns to ring 3 where it stopped.

Events, a suspend that meets a blocking call or a fault:

- The thread is parked in `port_wait`; the suspend wakes it with InternalIrqRetry; it took no packet; it parks at the syscall exit with 31 in rax; after the resume the stub sees 31 and traps again, with the same arguments. The program never sees 31.
- The thread faults on a pager VMO and waits; the suspend ends the wait; `Thread::Current::page_fault()` answers Ok; the thread parks at the exception tail; after the resume the instruction runs again and faults again.

Events, kill: `task_kill(process)` (Destroy) calls `ProcessDispatcher::kill()`; every thread gets Kill, a suspended one included; each dies at its next way out; the last one makes the process Dead and Terminated.

Lock order: ThreadDispatcher lock, then the kernel thread's PI lock, as `kill()` already does; the token's lock is never held across `resume()`.

Reference: Zircon `ThreadDispatcher::Suspend()`, `Resume()`, `MakeRunnable()`, `Suspending()`, `Resuming()`, `SetStateLocked()` (`kernel/object/thread_dispatcher.cc`), `SuspendTokenDispatcher` (`kernel/object/suspend_token_dispatcher.rs`, C++ before `c221bf9dbe2`), `zx_task_suspend` and `zx_task_kill` (`vdso/task.fidl`, `kernel/lib/syscalls/task.rs`), `BLOCKING_SYSCALL` (`kernel/lib/userabi/vdso/syscall-wrappers.cc:14-22`), `TranslatePageFaultStatus()` (`kernel/vm/vmm.cc:48-79`).

## Alternatives considered
- Zircon's vDSO shape for the retry, a hidden raw stub and a C++ do/while wrapper per blocking call: brings C++ into a vDSO that is one assembly file today, for the same behaviour.
- The kernel restarting the call itself by moving the user rip back onto the `syscall`: not researched far; it would retry calls the program did not mark.
- Bits alone instead of a Suspended state: fewer lines, but the signal bits become the only record of suspension, and a later thread-state read asks the state.
- Testing suspend through a job debugger instead of `thread_start`: reaches only a thread blocked in its start report, never one in `port_wait`, which is the case that tests the retry.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where `thread_start` and `thread_exit` land | here, before `task_suspend` (user-pager step 8 then done here); keep them at user-pager step 8 | here | None (homework paused) | here, 2026-10-23 |
| D2. Does `task_kill` take a thread | Zircon: process only, a thread and a job answer NotSupported, and the design row of `docs/design/260923/userspace-interface.md` is amended; this project's design: a thread too | Zircon | None | Zircon, 2026-10-23 |
| D3. Does `task_suspend` take a process | thread only, a process answers NotSupported; process too, with its own count | thread only | None | thread only, 2026-10-23 |
| D4. How the vDSO retries | an assembly loop in the generated stub; Zircon's C++ wrapper per call | the assembly loop | None | the assembly loop, 2026-10-23 |
| D5. Where the fault path translates the retry | now, in `mm::handle_vm_fault`; inline in the x86 handler; later, with the pager | now, in `handle_vm_fault` | None | now, 2026-10-23; then, at the user's word, `handle_vm_fault` removed and the translation put in `Thread::Current::page_fault()` |
| D6. A Suspended lifecycle state on ThreadDispatcher | yes, as Zircon; signal bits alone | yes | None | yes, 2026-10-23 |
| D7. Does a thread that exits by its own hand pass Dying | yes, as Zircon's `ExitingCurrent()` and this project's design row; no, Running to Dead as today | yes | None | yes, 2026-10-23 |

## Testing and verification
- kshell: suspend and kill a thread in State::Initial; it runs, and its interruptible sleep answers the signal (a kernel thread never runs `process_pending_signals()`, so the test thread has to observe the signal itself).
- vDSO: the generated stubs of the blocking calls, and only those, contain the retry (disassembly of the vDSO).
- Shell, both images, with a second thread from `thread_start`: the thread raises ThreadRunning and, after `thread_exit`, Terminated; `task_suspend` stops a thread spinning in ring 3 (a counter it bumps stops), the token's close restarts it; a thread parked in `port_wait` is suspended and resumed and then still receives the packet queued for it, with no 31 seen; the thread itself asks to suspend its own handle and gets NotSupported; `task_kill` of a child process with a suspended thread ends it. A second `thread_start` answers BadState and the thread runs normally (after `docs/repairs/261023/object.md` item 1).
- `ours-verify` on every step.

Cannot be verified: that a suspend landed while the thread was parked rather than before it parked, without a thread-state query or a kernel counter; the test uses a handshake through a channel to make it likely, not certain.

## Open questions
- Whether `thread_start` keeps the five-argument shape (entry, stack, two arguments) as the raw call, or follows Zircon's `thread_start_regs` with a thread pointer, which ours could add later behind the same program-facing call when thread-local storage arrives.
