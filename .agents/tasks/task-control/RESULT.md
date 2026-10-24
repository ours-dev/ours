# task-control: a program starts, suspends, resumes and kills threads, and sees it happen

**Date**: 2026-10-23

## Delivered
- `12e977e4`, `580f31ab`, `102bd1e1` docs: the design and plan, what the self-deadlock audit found, and decisions D1 to D7, all as recommended.
- `49ea4457` fix(object): a refused start no longer touches the thread's entry (step 1). Record: `docs/changes/261023/object.md`.
- `0180abc9` fix(task): a thread suspended or killed before it ran is started (step 2). Record: `docs/changes/261023/kernel-task.md`.
- `5e0f2aac` feat(abi): a blocking call's stub makes the call again on InternalIrqRetry (step 3). Record: `docs/changes/261023/abi.md`.
- `b37918f5` fix(task): a ring-3 fault whose wait a suspend or kill ended is not fatal, and `mm::handle_vm_fault` is gone (step 4). Record: `docs/changes/261023/kernel-task.md`.
- `c9588747` feat(object): a thread raises ThreadRunning, ThreadSuspended and Terminated (step 5). Record: `docs/changes/261023/object.md`.
- `b6e1ecb8` feat(syscall): `thread_start` and `thread_exit` (step 6). Record: `docs/changes/261023/syscall.md`.
- `4e80949f` feat(object): `task_suspend` and the suspend token (step 7). Record: `docs/changes/261023/object.md`.
- `f6e6286d` feat(syscall): `task_kill` ends a process (step 8). Record: `docs/changes/261023/syscall.md`.

## Verification
- `sleep.test` (kshell): a kernel thread suspended or killed before it ran answers InternalIrqRetry or InternalIrqKilled from its sleep; with step 2 reverted, both fail.
- vDSO disassembly: the seven blocking stubs, and only those, retry on 31.
- Shell, both images: `thread_start`, `task_suspend` and `task_kill` checks pass. A thread suspended before it starts, one counting in ring 3 and one parked in `port_wait` all stop and run again; the last receives its packet with Ok, the retry seen in a temporary log line.
- Shell, dbg_1: `task_kill probe checks passed`, for a child blocked in a system call, one whose only thread is suspended, one waiting for its own exception, and one killing itself.
- Control runs, each with the fix reverted: the Initial suspend (step 2), the thread signals (step 5) and the kill of a suspended thread (step 8) all fail their checks.
- `ours-verify` at the last step: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not exercised: step 4's translation of a ring-3 fault, which needs a thread suspended while it waits on a page source, so a user pager.
- Performance: not measured.

## Deviations from the design
- D5: the translation went into `Thread::Current::page_fault()`, and `mm::handle_vm_fault` was removed, at the user's word.
- The design's "task_kill of a child process with a suspended thread" runs on dbg_1 only, since the dev shell cannot launch a program. The thread comes from a job debugger channel's ProcessStarting, because the launcher closes the handles of the threads it starts.

## Found along the way
- `docs/repairs/261023/kernel-task.md` item 1: a wait that masks Suspend still answers it when another signal is pending; fixing it departs from Zircon, so it is the user's decision.
- `docs/repairs/261023/kernel-task.md` item 2: priority inheritance takes a wait queue's lock it already holds; latent, nothing calls it yet.
- `docs/repairs/261004/init.md` item 2 now notes that `task_kill` exists for a process but not for a job.

## Not done
- `task_kill` of a job or a thread, and `task_suspend` of a process (D2, D3).
- A thread-state query, without which the tests read where a thread stopped from the log.
- `thread_start`'s thread-pointer argument, `thread_read_state` and `thread_write_state` (DESIGN.md, Open questions).
- Renaming InternalIrqRetry and InternalIrqKilled: raised with the user, not decided.
- `.agents/tasks/user-pager/` resumes at its step 3.
