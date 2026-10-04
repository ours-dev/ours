# exception-channels: slice 1 delivered, a process exception channel a supervisor can pass on or resume through

**Date**: 2026-10-04

## Delivered
Slice 1 of `DESIGN.md`, in the six steps of `PLAN.md`, plus one style change the user asked for on the way:

- `2011cb6a` style(kernel): `arch::AutoInterruptEnabler`; paired state changes are scoped with an RAII object (the user's rule, now in `modern-cpp-development`).
- `51d4aa26` step 1: a user exception's delivery answers Resume or End; `deliver_user_exception()` is no longer `[[noreturn]]`.
- `c0e301e9` step 2: the ABI in `system/public` (`<ours/exception.hpp>`, `<ours/object_property.hpp>`, `ObjectType::Exception`).
- `edd1ba10` step 3: `ExceptionDispatcher`, `Exceptionate` on `ProcessDispatcher`, shut down first at process death; `Handle::make_uncharged()`.
- `a76049c7` step 4: `task_create_exception_channel` and `object_set_property(ExceptionState)`.
- `e0d900df` step 5: delivery, the send under the thread's lock, the kill-wins check after the wait; `userspace-interface.md` 4.8's lock order amended.
- The commit that adds this file: step 6, the shell checks every probe's exception type and resumes `trap_outside_vdso`.

## Verification
- Every step: `boot_check.py` on an isolated copy of HEAD with only that step's change (the shared worktree carried other sessions' unfinished work), both images PASS, 0 error lines. At step 6: dbg 24 passed, rel 21 passed.
- dbg_1, step 6: 16 fault probes each delivered exactly one exception of the expected type; 15 were passed on and killed with -1028, `trap_outside_vdso` was handled and survived (retcode 1, answered "survived"); the control probe delivered none; `shell: exception channel checks passed`.
- `waitqueue.test`, `sleep.test`, `threadjoin.test` after step 5: ok.
- clang-tidy on every changed line: no finding.
- Not measured: the cost of a channel exception round trip. Not exercised: a kill while a thread waits on its handler (nothing can kill another process's thread before `task_kill`), and lockdep (it does not link, `docs/repairs/260917/kernel-task-sched.md` item 6).

## Deviations from the design
- Step 1 passes a two-valued `task::ExceptionOutcome` (Resume, End) across the layers rather than three dispositions; which thread or process dies stays the object layer's business. `on_bad_syscall()` stays `void`.
- `Property` got a header of its own, `<ours/object_property.hpp>`; `ExceptionChannelOptions::Debugger` and `ExceptionState::ThreadExit` were left out until something uses them.
- `Exceptionate::send()` takes its `Arc` by value, because `ustl::Rc`'s constness is deep.
- The "in exception" mark was not added: nothing reads it in slice 1 (no `thread_read_state`, no Thread info topic). The send under the thread's lock still makes "still running" and "sent" one step against a kill.
- Step 5 had to change the shell too: a faulting probe now waits for its handler, so the shell drains the exception channel before it waits for the probe's answer.

## Found along the way
- The exception path printed the register dump for every fault; now only for one no handler saw, as Zircon does.
- `ours-verify` says to run kernel tests on both images, but the rel image boots with `kernel.shell=false` since `ead5d12f`; the skill and the comment in `kernel/products/image/dev/BUILD.gn` are stale. Not fixed here.

## Not done
- The thread slot and the walk thread before process, `exception_get_thread` / `_process`: slice 2, which the user asked for on 2026-10-04 as the base of a Starnix-like supervisor.
- Register access (`thread_read_state` / `thread_write_state`), the exception report with the syscall number, and making a bad syscall's rax final before a handler sees it: slice 2.
- The job slot, debugger channels, second chance, synthetic exceptions: later.
- Restricted mode: a separate track, after slice 2, as a performance question put to the user.
