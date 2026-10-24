# task-control: the steps, one commit each

**Date**: 2026-10-23
**Status**: Done, 2026-10-23 (see RESULT.md)

## Request
"我指的是一整套线程信号体系的接口": the interface through which a program starts, suspends, resumes and kills threads, and sees that it happened. Ends at DESIGN.md's goals; each step is introduced to the user before it starts. The user-pager task waits at its step 3 until this is done.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md D1 to D7 | | | | |

## Steps
1. object: a refused start no longer touches the thread's entry (`docs/repairs/261023/object.md` item 1). Reachable today through `process_start`, and a precondition of `thread_start`.
2. task: `suspend_or_kill()` starts a thread in State::Initial so it meets its signal (`docs/repairs/261023/kernel-task.md` item 1). A precondition of starting a thread suspended.
3. abi: a `blocking` call's stub retries InternalIrqRetry; `vmo_read`, `vmo_write` and `process_read_memory` become blocking; the generator refuses `blocking` with `noreturn`. Nothing returns 31 to ring 3 yet, so behaviour is unchanged; checked by disassembly.
4. task: `Thread::Current::page_fault()` translates a ring-3 fault's InternalIrqRetry and InternalIrqKilled; the unused `mm::handle_vm_fault` declaration is removed.
5. object: thread object signals (ThreadRunning, ThreadSuspended, Terminated raised outside the process lock).
6. syscalls: `thread_start` and `thread_exit`; shell `check_thread_start`. User-pager PLAN.md step 8 is marked done here.
7. object and syscalls: the suspend count and the Suspended state, `SuspendTokenDispatcher`, `task_suspend`; shell `check_task_suspend`. The first call that can suspend a thread, after its consumers in steps 3 and 4.
8. syscalls: `task_kill` for a process; shell `check_task_kill`; the `task_kill` row of `docs/design/260923/userspace-interface.md` amended per D2.

## Files
- `kernel/object/thread_dispatcher.*`, `kernel/object/process_dispatcher.cpp` — steps 1, 5, 7
- `kernel/kernel/task/thread.cpp` — step 2
- `build/abi/gen_syscalls.py`, `kernel/lib/userabi/vdso/vdso.S`, `system/public/ours/syscalls.gni` — step 3, and entries in steps 6 to 8
- `kernel/kernel/task/thread.cpp`, `kernel/mm/include/ours/mm/fault.hpp` (removed) — step 4
- `system/public/ours/signals.hpp` — step 5
- `kernel/lib/syscall/src/task.cpp`, `kernel/object/suspend_token_dispatcher.*` (new), `system/public/ours/types.hpp` — steps 6 to 8
- `system/app/shell/shell_tests.cpp`, `kernel/commands/` — tests

## Test plan
As DESIGN.md, Testing and verification. Each step: `ours-verify` passes and the tests it adds pass.
