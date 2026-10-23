# abi changes

## A blocking call's vDSO stub makes the call again when a suspend ended it (2026-10-23)

**Files**
- `kernel/lib/userabi/vdso/vdso.S` — the stub macro takes `blocking`; a blocking stub compares the result with InternalIrqRetry and traps again
- `build/abi/gen_syscalls.py` — passes the attr to the macro; refuses `blocking` on a call that does not return Status
- `system/public/ours/syscalls.gni` — what `blocking` promises; `vmo_read`, `vmo_write` and `process_read_memory` are blocking
- `system/public/ours/status.hpp` — what InternalIrqKilled and InternalIrqRetry are, and the value the stub compares with, pinned

**Symptom**
Latent until something can suspend a user thread (`.agents/tasks/task-control/` step 7): a suspend that ends a blocking call's wait makes the call return InternalIrqRetry (31), and the program would receive it from `port_wait`, `object_wait_one`, `interrupt_wait`, and, on a pager VMO, from `vmo_read`, `vmo_write` and `process_read_memory`.

**Cause**
InternalIrqRetry is the kernel's way to let a thread that was waiting stop where it is safe to, the syscall exit, and is meant to be consumed before the program sees it: the call is made again once the thread resumes. The `blocking` attr existed to mark exactly those calls, but the generator only validated it, and nothing generated the retry; and the three copy calls, which can now wait on a page source, were not marked.

**Change**
In the stub macro, for a blocking call:

```asm
.if \blocking
.Lretry_\name:
.endif
    mov $\number, %eax
    syscall
\sysret:
...
.if \blocking
    cmp $OURS_STATUS_INTERNAL_IRQ_RETRY, %eax
    je .Lretry_\name
.endif
```

The jump goes back past the argument moves and pushes, because the kernel restores every register it read an argument from (the exit path pops the whole syscall frame, and a call writes only the result), and it lands on the same `syscall`, so the trap address the kernel checks does not change. The value 31 is a `#define` in the assembly, with a `static_assert` in `status.hpp` against `Status::InternalIrqRetry`.

`syscalls.gni` now says what `blocking` promises: the call can be made again from the start, because an interrupted wait changed nothing or only what doing it again repeats. A call that changes something and then waits, a send followed by a wait for the reply, is not blocking and has to be split into a call and a finish, as Zircon splits `channel_call`.

Rejected: Zircon's shape, a hidden raw stub and a C++ do/while wrapper per blocking call, which would bring C++ into a vDSO that is one assembly file, for the same behaviour (the user's decision D4 of `.agents/tasks/task-control/DESIGN.md`).

**Reference**
Zircon `BLOCKING_SYSCALL` (`kernel/lib/userabi/vdso/syscall-wrappers.cc:14-22`) loops while the result is `ZX_ERR_INTERNAL_INTR_RETRY`; `zx_channel_call` (`zx_channel_call.cc:11-14`) is the split into `channel_call_noretry` and `channel_call_finish`; `@blocking` on `vmo_read`, `vmo_write` and `process_read_memory` in `vdso/vmo.fidl` and `vdso/process.fidl`.

**Verification**
- Disassembly of the linked vDSO: exactly seven stubs carry `cmpl $0x1f, %eax; je` after their `syscall` -- `debug_read`, `interrupt_wait`, `object_wait_one`, `port_wait`, `process_read_memory`, `vmo_read`, `vmo_write` -- and the other 38 do not. `port_wait`'s retry jumps to the stub's first instruction; `process_read_memory`'s to `+0x3`, past `mov %rcx, %r10`.
- The generator refuses `blocking` with a non-Status return and with `noreturn`, and accepts it on a Status call.
- `ours-verify`: dbg PASS (39 passed, 4 skipped), rel PASS (37 passed, 8 skipped, 6/6 required), 146 host tests pass. Behaviour is unchanged: nothing returns 31 to ring 3 yet. The retry itself is exercised at step 7, where a thread parked in `port_wait` is suspended and resumed.

**Deliberately not done**
`debug_read` keeps `blocking`: it busy-polls and never answers 31, so the retry is dead code there, but the attr says what the call may do.

**How it was found**
The task-control design: the consumers of InternalIrqRetry must land before the first call that can suspend a thread.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Why can the stub jump back to the `mov` instead of to the first instruction, and what would go wrong for a seven-argument call if it jumped back to the first?
2. Which property must a call have to be marked blocking, and why does `port_wait` have it though it is not idempotent?
3. Why does the retry land on the same `syscall` instruction, and what does the kernel check against that address?
