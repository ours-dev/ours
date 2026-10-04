# exception-channels: plan slice 1, a process exception channel

**Date**: 2026-10-04
**Status**: In progress

## Request
The user asked (2026-10-03) to bring in "the mechanism Starnix uses to let user space catch exceptions", and chose Zircon's exception channels (2026-10-04). This plan covers slice 1 of `DESIGN.md`: a supervisor binds a channel to a process, reads each exception's type, and either passes it on (the process is killed, as today) or marks it handled (the thread resumes). Register access, the thread and job slots, and restricted mode are outside it.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| All slice-1 decisions | See `DESIGN.md` | See `DESIGN.md` | | Decided 2026-10-04, recorded in `DESIGN.md` |

## Steps
Each step is introduced to the user before it starts, builds and boots both images, and lands as one commit.

1. **Resumable delivery.** `deliver_user_exception()` stops being `[[noreturn]]`; `ThreadDispatcher::on_user_exception()` answers a `task::ExceptionOutcome` (Resume or End; which thread or process dies stays the object layer's business) through `task::Thread::Current::on_user_exception()`. `on_bad_syscall()` stays `void`: `not_from_vdso()` answers BadSyscall whatever happens, as Zircon's `sys_invalid_syscall()` does. Every outcome is still End, so behaviour does not change. Verified by boot: every fault probe is still killed with -1028. Done 2026-10-04.
2. **ABI.** In `system/public`: `ObjectType::Exception = 29`; the public `ExceptionType` and `PolicyCode` (the kernel's `task/exception.hpp` keeps using them); `ExceptionInfo`; `ExceptionChannelOptions`; `Property` and `ExceptionState`. Zircon's values throughout. Verified by build and static_asserts. Done 2026-10-04; `Property` went to a header of its own, `<ours/object_property.hpp>`, and values slice 1 does not answer (Debugger, ThreadExit) were left out.
3. **The objects.** `ExceptionDispatcher` and `Exceptionate` in `kernel/object`; `ProcessDispatcher` gains its exceptionate, shut down first in `finish_dead_transition()`; a `Handle::make` form that charges nobody. Nothing delivers yet. Verified by boot. Done 2026-10-04.
4. **The system calls.** `task_create_exception_channel` and `object_set_property` (ExceptionState only). A channel can be bound; nothing is sent on it yet. Verified by boot and a shell check that a bound channel reports PeerClosed when the process dies. Done 2026-10-04: every fault probe gets one, with the second-bind and write refusals checked too.
5. **Delivery.** `ThreadDispatcher::on_user_exception()` marks the thread, sends under its lock and the exceptionate's (the amended order, written into `userspace-interface.md` §4.8 in this commit), waits interruptibly, checks Dying, and answers the disposition. `on_bad_syscall()` goes through it too. Done 2026-10-04, with the shell draining each probe's exceptions (TryNext) before its answer, which delivery made necessary.
6. **The test.** `run_probe()` binds a channel per probe, checks each exception's type, passes it on, and marks `trap_outside_vdso` handled; the shell prints `shell: exception channel checks passed`. Done 2026-10-04.

Slice 2, thread-level capture for a Starnix-like supervisor (asked for on 2026-10-04):

7. **The faulting tasks.** `exception_get_thread` / `exception_get_process`, with the task rights an exceptionate records at bind time. Done 2026-10-04.
8. **The thread slot.** An Exceptionate on ThreadDispatcher, tried before the process's; `task_create_exception_channel` takes a thread handle. How a supervisor gets a guest's thread handle before it runs is a decision for the user, shared with the launcher work. The user chose a two-phase start (C, 2026-10-04), so the step is split:
   - 8a. The slot, its shutdown at thread exit, and the system call; the shell binds it from an exception's thread. Done 2026-10-04.
   - 8b. The two-phase start: the launcher creates the process and its first thread without starting them, the caller binds what it wants and then calls `process_start`, as Zircon's `fuchsia.process.Launcher.CreateWithoutStarting` does. The shell then checks that a thread's channel is told before its process's. Waits for the launcher work in flight in another session.
9. **Register access.** `thread_read_state` / `thread_write_state` through a per-thread pointer to the user frame while the thread is in an exception, with rip, cs/ss and rflags validated.
10. **A bad system call's registers.** rax final (BadSyscall) before a handler sees the exception, so its writes survive; Zircon defers the policy exception to syscall exit. A decision for the user.
11. **The exception report.** `object_get_info(ThreadExceptionReport)`, carrying the system call's number.
12. **A mini-Starnix test.** A probe makes a raw system call with a made-up number; the supervisor emulates it and writes the result into rax.

## Files
- `kernel/arch/x86/kernel/faults.cpp`: step 1.
- `kernel/include/ours/task/thread.hpp`, `kernel/kernel/task/thread.cpp`: step 1, the disposition through the task layer.
- `kernel/object/include/ours/object/thread_dispatcher.hpp`, `kernel/object/thread_dispatcher.cpp`: steps 1 and 5.
- `kernel/lib/syscall/src/table.cpp`: step 1.
- `system/public/ours/types.hpp` and a new public exception header, `system/public/public_headers.gni`: step 2.
- `kernel/include/ours/task/exception.hpp`: step 2, the types move to the public header.
- New `kernel/object/exception_dispatcher.cpp`, `kernel/object/exceptionate.cpp` and their headers, `kernel/object/BUILD.gn`: step 3.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp`, `kernel/object/handle.cpp`: step 3.
- `system/public/ours/syscalls.gni`, new bodies in `kernel/lib/syscall/src`: step 4.
- `docs/design/260923/userspace-interface.md`: step 5, the lock order.
- `examples/shell/shell_tests.cpp`: steps 4 and 6.

## Test plan
- Every step: `boot_check.py` on both images, `PASS` on each; dbg_1 still shows every fault probe killed (`killed (retcode -1028)`) until step 6 changes what it expects.
- Step 6, on dbg_1: one line per probe with its exception type, `trap_outside_vdso` surviving with SURVIVED, and `shell: exception channel checks passed`.
- `waitqueue.test` and `threadjoin.test` after step 5, which adds a new kind of interruptible wait.
