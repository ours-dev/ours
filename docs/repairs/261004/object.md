# object outstanding repairs

## 1. The shell cannot check an interrupt wait that parks

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

## 2. A cross-CPU handoff through a virtual interrupt takes about 4 ms on dev.dbg_1, and once took ten times that

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

## 3. A full channel still answers NoResource, though D11's switch to a policy exception is now possible

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
