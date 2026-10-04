# exception-channels: plan slice 1, a process exception channel

**Date**: 2026-10-04
**Status**: Done

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
