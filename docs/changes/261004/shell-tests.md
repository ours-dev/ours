# shell-tests changes

## Title: The shell checks each fault probe's exception type and resumes the one it can (2026-10-04)

**Files**
- `examples/shell/shell_tests.cpp` — the probe list gains each probe's expected exception type and whether the shell handles it; `pass_on_exceptions()` marks `trap_outside_vdso`'s exception Handled; each probe's line names its exception, and the verdict counts type and ending together.
- `.agents/tasks/exception-channels/PLAN.md`, `.agents/tasks/exception-channels/RESULT.md` — step 6 done, slice 1 delivered.

**Symptom**
Before: every probe's exception was delivered and passed on, but nothing checked its type, and nothing had ever taken the Handled path. Step 6 of `.agents/tasks/exception-channels/`, the last of slice 1.

**Cause**
"The supervisor receives an exception" and "the exception is the right one" are different claims, and only the second catches a mapping slip in `faults.cpp` (a #GP reported as a page fault, say). And Handled, the one disposition that brings a thread back, had kernel code behind it (`deliver_exception()` answering Resume, `deliver_user_exception()` returning to the exception tail) and no caller.

Only `trap_outside_vdso` can be resumed without touching registers. A system call from outside the vDSO is refused, not faulted: on Handled the thread returns from `not_from_vdso()`, the call answers BadSyscall, and execution continues after the `syscall` instruction. A fault resumed unchanged re-executes the faulting instruction and faults again, which needs `thread_write_state` (slice 2) to step over.

**Change**
The probe table, with the types the kernel reported in step 5's log:

| Probes | Type |
|---|---|
| `pf_*` | FatalPageFault (0x108) |
| `ud2` | UndefinedInstruction (0x208) |
| `gp_hlt`, `gp_int80`, `gp_io`, `de`, `gp_int3` | General (0x8) |
| `db_tf` | HwBreakpoint (0x408) |
| `trap_outside_vdso` | PolicyError (0x8208), handled |

`gp_int3` is General because the #BP gate is DPL 0, so a ring-3 `int3` raises #GP; `de` is General because a divide error has no kind of its own. A probe counts as expected when its channel delivered exactly one exception of its type and it ended as it should: killed with -1028, or, for the handled one, returned 1 after answering "survived" (`ExitCode::SURVIVED` in `fault_probe.cpp`).

**Reference**
Zircon's own exception tests resume a thread the same way, with `ZX_EXCEPTION_STATE_HANDLED` set on the exception before the handle is closed (zircon/system/utest/core/exceptions/user-exceptions.cc).

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (24 passed), rel PASS (21 passed), 0 error lines.
- dbg_1's log: `shell: probe trap_outside_vdso handled exception 0x8208 and survived (retcode 1, answered "survived")`; the other 15 `killed (retcode -1028) after exception <type>`, each the expected type; `exception channels: 16 probes delivered their exception and ended as expected, none for the control`; `exception channel checks passed`.
- clang-tidy: no finding on the changed lines.

**Deliberately not done**
- Resuming a fault: needs register access, slice 2.
- Checking `ExceptionInfo::pid` and `tid` against the probe's koids: needs `object_get_info(HandleBasic)`, which answers NotSupported today.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `trap_outside_vdso` the only probe that can be resumed in slice 1?
- What does the probe's thread execute next after its exception is marked Handled, and what does its `syscall` return?
- Why is `gp_int3` expected as General rather than SwBreakpoint?
