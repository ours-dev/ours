# object outstanding repairs

## 1. Two threads can both park on one interrupt, and destroy then wakes only one of them

**Severity**
Medium: a thread can stay parked in `interrupt_wait` on a destroyed interrupt, against the call's documented "a thread waiting on it wakes with Cancelled". It needs two threads waiting on one interrupt at once, which the call answers BadState for in every other interleaving; today that takes two processes holding duplicates of one handle, or two kernel threads. Zircon has the same race. Found by reading the code in the review of step 2, not reproduced.

**Symptom**
Latent. After the interleaving below, `interrupt_destroy` (or closing the last handle) returns Cancelled to one parked waiter and leaves the other parked until its thread is killed. A later trigger likewise wakes only one of the two. Since interrupts can be bound to ports (2026-10-04), a bind can land after the trigger that wakes one of them: the woken one answers BadState, and `destroy()` of a bound interrupt signals no one, so the other stays parked even through the destroy.

**Cause**
The objects:
- `m_state` and `m_timestamp` of `InterruptDispatcher`, under L2 (`m_spinlock`). Waiting is the only record that a thread is waiting, and the next trigger overwrites it with Triggered.
- `m_event`, an auto-unsignal event: `signal()` wakes one parked thread, or latches when none is parked.
- `begin_wait()` unsignals `m_event` under L2 whenever it takes an interrupt (`kernel/object/interrupt_dispatcher.cpp`, the Triggered and NeedAck cases).

The events, with threads A and B waiting on the same interrupt:
1. A: `begin_wait()` finds Idle, sets Waiting, drops L2. It is not on the event's queue yet.
2. A trigger finds no one parked, latches the signal, and sets Triggered.
3. B: `begin_wait()` finds Triggered, takes the interrupt, sets NeedAck, and unsignals: the latch that would have woken A is gone.
4. B waits again: NeedAck with nothing pending, so it sets Waiting and parks.
5. A reaches `park()`, finds no latch, and parks too. Two threads are parked; the state says Waiting.
6. `destroy()` sets Destroyed and signals once. The event wakes one thread, which answers Cancelled. The other stays parked: nothing will signal again, and `on_zero_handles()` finds the object already destroyed.

The "one waiter at a time" rule is enforced by the state alone, and event 2 overwrites the only record of A.

**Change**
Options, for the user to choose, since each departs from Zircon:
- (a) Keep the waiter in its own field under L2, set by `begin_wait()` when it returns none and cleared under L2 when `park()` returns, and answer BadState to any `begin_wait()` while it is set. Event 3 then still lets B take the interrupt, but event 4 answers BadState, so only A parks. The rule becomes an invariant held under L2 instead of a reading of the state. Recommended.
- (b) Make `destroy()` release every parked thread: an `Event` operation that wakes all waiters whatever the mode. It fixes destroy only; a trigger still wakes one of two.
- (c) Leave it as Zircon has it and document that a second waiter is undefined.

**Reference**
Zircon `InterruptDispatcher::BeginWaitForInterrupt()` and `Destroy()` (`zircon/kernel/object/interrupt_dispatcher.cc:47-119`, `:259-297`), which have the same race: TRIGGERED returns `event_.Unsignal()`, and `Destroy()` calls `Signal()` once on an `AutounsignalEvent`.

**Acceptance**
A kernel test in `kernel/commands/interrupt_test.cpp` that forces events 1-5, for example with a test hook between `begin_wait()` and `park()`, and shows `destroy()` waking both threads with Cancelled (b), or the second wait answering BadState (a); `interrupt.test: ok` on dev.dbg_1.

## 2. The shell cannot check an interrupt wait that parks

**Severity**
Low: a missing test. The parked paths are covered by the kernel's `interrupt.test` on dev.dbg_1, but not through the system calls.

**Symptom**
`check_interrupt()` in `examples/shell/shell_tests.cpp` checks only waits that return without parking. That a trigger wakes a parked `interrupt_wait`, and that `interrupt_destroy` wakes it with Cancelled, is checked on kernel threads only.

**Cause**
A program cannot start a second thread: `thread_create` makes one, but only `process_start` starts one, as the first thread of a new process. The checks need one thread parked in `interrupt_wait` while another triggers or destroys.

**Change**
Once a program can start a thread of a running process (Zircon's `zx_thread_start`), add to `check_interrupt()`: two threads trigger and wait in turn through two virtual interrupts, each wait returning the time of its trigger; and a destroy wakes a parked waiter with Cancelled.

**Reference**
Zircon's `InterruptTest.WaitThreadFunctionsAfterSuspendResume` (`zircon/system/utest/core/interrupt/interrupt-test.cc`), which starts a second thread with `zx::thread::start` to block in `zx_interrupt_wait` while the test thread triggers.

**Acceptance**
`shell: interrupt checks passed` on both images, with the two-thread checks in it.

## 3. A cross-CPU handoff through a virtual interrupt takes about 4 ms on dev.dbg_1, and once took ten times that

**Severity**
Low: a performance observation on the dbg image, not measured on its cause.

**Symptom**
`interrupt.test`'s ping-pong, two kernel threads on CPUs 0 and 1 taking turns through two virtual interrupts, ran 1000 rounds in 7878 ms on 2026-10-04: about 8 ms a round, two wakeups each. One earlier run took about 48 s for 500 rounds. Each round logs several `IPI-Resched` trace lines to the serial console.

**Cause**
Not known. Candidates: the serial console's cost for the trace lines of every reschedule IPI, the latency of waking an idle CPU, or QEMU's TCG. None was measured.

**Change**
Measure first, as CLAUDE.md asks for performance work: the time from `trigger()` to the woken thread running, with and without trace-level logging, on the dbg and the rel kernel, and its distribution over a thousand rounds. Then decide with the user whether there is anything to fix.

**Reference**
None, a measurement.

**Acceptance**
A recorded measurement that explains the handoff time, and the user's decision on it.

## 4. A full channel still answers NoResource, though D11's switch to a policy exception is now possible

**Severity**
Low: a planned step not yet taken. Today's answer is correct by D11 (a) and bounds kernel memory; it only lets a writer that ignores NoResource keep going.

**Symptom**
`ChannelDispatcher::write_self_locked()` (`kernel/object/channel_dispatcher.cpp`) answers NoResource once `CHANNEL_MAX_PENDING_MESSAGES` (64) messages wait on the peer's end, and nothing else happens to the writer.

**Cause**
Design D11 (`docs/design/260923/userspace-interface.md`, the decision table) chose (a), NoResource with WRITABLE left set, and planned to move to (c), a policy exception on the writing thread, once the exception mechanism landed. It has: `ThreadDispatcher::on_policy_exception()` raises a PolicyError through the exception channels, and `on_bad_syscall()` already uses it for `PolicyCode::BadSyscall`. Nothing tracked the switch, and the comment on `CHANNEL_MAX_PENDING_MESSAGES` still said it waited for an exception mechanism until the comment review of 2026-10-04 corrected it.

**Change**
For the user to decide, since it changes what a program sees:
- (a) Make the switch D11 planned: a new `PolicyCode` for a write to a full channel, raised from the channel write on the calling thread, outside the PeerHolder lock, through `on_policy_exception()`; the write still answers NoResource for a thread a handler resumes. Recommended, as D11 planned.
- (b) Keep NoResource for good, and record in D11 why the plan changed.

**Reference**
Zircon `ChannelDispatcher::WriteSelf()` (`zircon/kernel/object/channel_dispatcher.cc:629-638`), which calls `Thread::Current::SignalPolicyException(ZX_EXCP_POLICY_CODE_CHANNEL_FULL_WRITE, 0u)` past `kMaxPendingMessageCount`.

**Acceptance**
For (a): a shell check that writes past the limit with no handler bound and sees the process killed with the exception return code, and one with a handler bound that receives a PolicyError with the new code; `ours-verify` passes on both images. For (b): D11's row says the plan was dropped, and why.
