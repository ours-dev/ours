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
