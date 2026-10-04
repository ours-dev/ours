# exception-channels: let a user-space supervisor catch another process's exceptions, as Zircon's exception channels do

**Date**: 2026-10-04
**Status**: Accepted

## Problem
Every ring-3 exception kills its process. `x86_dispatch_exception()` (`kernel/arch/x86/kernel/faults.cpp`) builds a `task::ExceptionContext` whose types already carry Zircon's `ZX_EXCP_*` values (`kernel/include/ours/task/exception.hpp`), and `deliver_user_exception()`, which is `[[noreturn]]`, hands it to `ThreadDispatcher::on_user_exception()`. That function logs, dumps the registers and calls `ProcessDispatcher::kill(TASK_RETCODE_EXCEPTION_KILL)`. A system call from outside the vDSO takes the same path, synchronously, from `not_from_vdso()` (`kernel/lib/syscall/src/table.cpp:87-102`). No program can learn which fault another program took, except from the kernel log and a return code, and no fault can be survived.

The user asked on 2026-10-03 for "the mechanism Starnix uses to let user space catch exceptions". Starnix itself is Fuchsia's Linux compatibility layer. Today it catches its Linux program's system calls and faults with restricted mode (RFC-0261). Until 2023 it did so with exception channels, the general mechanism that debuggers and crash reporters use, and it still uses them for its user-copy fault fixer. The exploration (2026-10-03, verified against the Fuchsia tree at 51f05104) laid out four options: exception channels, restricted mode, Zircon's full handler chain, and Linux-style signals. The user chose exception channels on 2026-10-04 (option A), which moves them ahead of their P2 slot in `docs/design/260923/userspace-interface.md:103,587`. That design deferred them because init can supervise through TERMINATED, the return code and the kernel's fault report. What a channel adds is an in-band, per-fault record of what happened, and a way to let the faulting thread go on.

## Goals and non-goals
Goals, slice 1:
- A supervisor binds a channel to a process with `task_create_exception_channel`. For each exception of a thread in it, the supervisor reads an `ExceptionInfo {pid, tid, type}` and an exception handle.
- Closing the handle passes the exception on (TRY_NEXT). With no other handler, the process is killed exactly as today, with `TASK_RETCODE_EXCEPTION_KILL`.
- Setting `ExceptionState::Handled` on the handle with `object_set_property` before closing it resumes the thread. For an out-of-vDSO system call, the call returns `BadSyscall` and the program goes on.
- The channel reports PeerClosed once the process is dead.
- The kernel log keeps its fault report for an exception nobody handled.

Non-goals for slice 1, each addable later without changing a struct, a number or a signature:
- The thread slot, the job slot and the job walk, the process-debugger slot and second chance (`ZX_PROP_EXCEPTION_STRATEGY`).
- `exception_get_thread` and `exception_get_process`; `object_get_property`; `ExceptionState::ThreadExit`. None has a consumer in slice 1's test.
- Register access (`thread_read_state` / `thread_write_state`), the exception report (`get_info(ThreadExceptionReport)`), and the deferred policy exception that register access needs. These are slice 2, the "mini-Starnix" that emulates a raw system call.
- Synthetic exceptions (THREAD_STARTING and the like), `zx_thread_raise_exception`, `task_kill`, `task_suspend`.
- Restricted mode. It is a second track after this one, and reuses this slice's resumable delivery.

## Design
Objects. Zircon's names are kept, as the kernel keeps them elsewhere.
- **ExceptionDispatcher** (`kernel/object`, `ObjectType::Exception = 29`, `SoloDispatcher`). It is one exception in flight. It holds an `Arc<ThreadDispatcher>`, the type, the pid and the tid (copied in, not pointing into the faulting thread's stack, so Zircon's `Clear()` hazard does not arise until a report call exists), the disposition (default TryNext), and the response event. `on_zero_handles()` signals the event: the exception handle's count reaching zero is how the handler says it is done. Its default rights are `Transfer | GetProperty | SetProperty | Inspect`, Zircon's `ZX_DEFAULT_EXCEPTION_RIGHTS`. Without Duplicate there is only ever one handle, and without Wait it cannot be waited on.
- **Exceptionate** (`kernel/object`, not a kernel object), a member of `ProcessDispatcher`. It holds the kernel end of one channel (`KernelHandle<ChannelDispatcher>`), the task rights to hand out, an `is_shutdown` flag, and its own lock. Its operations are `set_channel()` (AlreadyBound, or BadState after shutdown), `send()` and `shutdown()`.
- **The channel**: an ordinary `ChannelDispatcher` pair. The user end's rights are masked to `Transfer | Wait | Read`.
- **The message**: `ExceptionInfo` (24 bytes, Zircon's `zx_exception_info_t` layout) and exactly one handle, the exception's.

Events, for a fault of thread T in process P whose exceptionate S has a channel:
1. The arch builds the context, as today, and calls up through `task::Thread::Current::on_user_exception()` into `ThreadDispatcher::on_user_exception()`, which now returns whether to resume. `deliver_user_exception()` stops being `[[noreturn]]`.
2. T creates an ExceptionDispatcher E and marks itself "in exception" under its ThreadDispatcher lock, unless it is Dying (Zircon's `exception_` and `Blocked::EXCEPTION`; where the mark lives is an open question).
3. Still holding its ThreadDispatcher lock, as Zircon's `HandleException()` does, T asks S to send: under S's lock, mint the exception handle, build the packet, write it on the kernel end (taking PeerHolder). The mark and the message become visible as one step for anyone who takes T's lock. T then releases its lock.
4. T waits on E's event, interruptibly, with Suspend masked: Zircon's `ExceptionDispatcher::WaitForHandleClose()`.
5. The supervisor reads the message, may set `Handled`, and closes the handle; `on_zero_handles()` signals E.
6. T wakes, takes its ThreadDispatcher lock, and if it is Dying ignores the disposition (a kill always wins: Zircon `thread_dispatcher.cc:699-712`). Otherwise it reads the disposition and clears the mark. Handled returns to ring 3 through the existing tail of `arch_handle_exception()` (`process_pending_signals()`, then `iretq`). TryNext means no handler is left in slice 1, so the process is killed as today.
7. No channel on S, or a send that fails (PeerClosed, NoResource, no handle slot), counts as TryNext, as in Zircon's `ExceptionHandlerIterator`.

A bad system call follows the same walk from `not_from_vdso()`. On Handled it returns, and `not_from_vdso()` writes `BadSyscall` into rax as it does today, so the program sees a refused call rather than death. Deferring it to the syscall exit, as Zircon does, is needed only once a handler can write registers (slice 2).

Process exit: `finish_dead_transition()` shuts S down first, as Zircon's `ProcessDispatcher` does, outside the process lock (hard rule 1 of `userspace-interface.md` §4.8). That drops the kernel end, so the supervisor sees PeerClosed, and it breaks the reference cycle P → S → kernel end ⇄ user end → queued packet → exception handle → E → T → P. A `set_channel()` after the shutdown answers BadState, under S's lock.

System calls (entries in `system/public/ours/syscalls.gni`, bodies in `kernel/lib/syscall`): `task_create_exception_channel(Uandle task, ExceptionChannelOptions options, Uandle *out)` for a process handle, with Zircon's rights check (`Inspect | Duplicate | Transfer | ManageThread`, plus `Enumerate` for a process); `object_set_property(Uandle handle, Property property, void const *value, usize size)` for `Property::ExceptionState` only, answering NotSupported for any other property.

The resume path already holds: the frame lives on T's own kernel stack (`deliver_user_exception()` asserts it), and the interrupt stub reloads every register from it before `iretq`.

## Alternatives considered
- **Restricted mode first** (exploration option C). It is fast, about 0.5 us per round trip against about 6.8 us for a channel exception in Zircon's 2023 measurement, and it is what Starnix runs on today. Rejected as the first step by the user: it needs a departure from the vDSO-only rule, an address-space split and SSE before it serves a real program, and its only consumer would be a demo. This slice's resumable delivery is a prerequisite of it.
- **Zircon's whole handler chain at once** (option B). Rejected: most pieces (job tree, debugger slots, suspend, register access) have no ring-3 consumer yet, against the rule that a userspace requirement drives each piece.
- **Linux-style in-process signals** (option D). Rejected: no Zircon spec to follow, cannot supervise another process, and bypasses handles and rights.
- **A dedicated system call to set the disposition** instead of `object_set_property`. Rejected: Zircon's way is a property, and the property call is already a P1 row of the accepted design; a dedicated call would be a stand-in.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Which mechanism, and whether now | A exception channels; C restricted mode; B Zircon's whole chain; D Linux signals; or leave it at P2 | A, now | | A, 2026-10-04 |
| Slice 1's scope | The trimmed slice above (process slot, two system calls, TryNext and Handled); or the exploration's wider one (thread slot, `exception_get_thread/process`, `object_get_property`, ThreadExit) | Trimmed: everything in it has a consumer in the test | | As recommended, 2026-10-04 |
| Where the send happens relative to the ThreadDispatcher lock | (a) Zircon: hold T's lock across the send, so "in exception" and "message visible" become true together; the send then takes PeerHolder after a Dispatcher lock, against the documented order (`userspace-interface.md` §4.8). (b) Mark T under its lock, release it, then send under S's lock only | (b): marking first already makes the mark visible before the message, and S's lock sits between Process and PeerHolder in the documented order | | (a), 2026-10-04: follow Zircon. Checked first: no path takes a PeerHolder and then a ThreadDispatcher lock today (channels free packets outside the holder lock, `channel_dispatcher.cpp:59-71`, `dispatcher.hpp:223-234`; `ThreadDispatcher` has no `on_zero_handles`). The order becomes Job → Process → ThreadDispatcher → Exceptionate → HandleTable → PeerHolder → other Dispatcher locks, and no code may take a ThreadDispatcher lock while holding a PeerHolder. Nothing enforces that mechanically until lockdep links (`docs/repairs/260917/kernel-task-sched.md` item 6) |
| Who is charged for the minted exception handle | (a) the faulting process, as `Handle::make()` does today; (b) nobody; (c) the supervisor | (b): at most one exception handle per blocked thread, so it cannot be used to exhaust a quota, while under (a) a process at its quota cannot have its exception delivered and dies silently. A departure from design §4.8, which charges every live handle to its creator | | (b) nobody, as recommended, 2026-10-04 |
| How an exception is delivered | A channel message per exception, Zircon since 2019; or a port packet, Zircon before 2019 (removed in f493c2cbb31) | Channel: Zircon's current form; a supervisor of several processes waits on several channels until `object_wait_many` or ports exist | | Channel, as recommended, 2026-10-04 |
| The exception object's lifetime | Allocated per exception, as Zircon does; or one preallocated per thread | Per exception: faults are not a hot path, and a failed allocation counts as TryNext. Preallocation would remove an allocation per round trip if channel exceptions ever carry emulated system calls | | Per exception, as recommended, 2026-10-04 |

Per CLAUDE.md, the lock-order decision needs the user's written argument first: which invariant it protects, who reaches it concurrently, and the worst interleaving.

## Testing and verification
- `examples/shell/shell_tests.cpp`, `run_probe()`, on dbg_1. Bind a channel to each `fault_probe` process after `launcher()` returns and before the processargs are written. Check `ExceptionInfo::type` per probe: `pf_*` 0x108, `ud2` 0x208, `gp_*` and `de` 0x008, `db_tf` 0x408, `trap_outside_vdso` 0x8208. Close the handle, and expect the kill and the channel's PeerClosed. For `trap_outside_vdso`, set Handled and expect the probe to survive. Print `shell: exception channel checks passed` or a FAILED line, so that `boot_check.py` sees the verdict.
- The control probe `none`: the channel reports only PeerClosed.
- `ours-verify` on both images; the probes run only on dbg_1.
- Not verifiable in slice 1: a kill racing the wait, since nothing can kill another process's thread before `task_kill` exists. It is argued, and the wake path it relies on is covered by `waitqueue.test`.

## Open questions
- `launch::start()` starts the process before it returns. A fault before `ort` blocks on its processargs finds no channel bound yet, and is reported only in the log. Binding before the start needs a launch-request option, which the launcher-service design has to choose before it lands.
- `ExceptionInfo` carries no synth code, so the test cannot check that a policy exception is BadSyscall (18) until the report exists (slice 2).
- Whether `object_set_property` comes now as the general P1 property mechanism (Name, fs/gs base) or as ExceptionState only.
- Where the "in exception" mark lives. `ThreadDispatcher` has an empty `ParkedReason` enum and an `m_parked_reason` that nothing reads, which look like scaffolding for Zircon's `Blocked` reasons; no document says so, and scaffolding is not repurposed without asking.
- `docs/design/260923/userspace-interface.md` §4.8 states the total order this slice amends (ThreadDispatcher before HandleTable and PeerHolder). The amendment is written there in the change that first takes the new order.
