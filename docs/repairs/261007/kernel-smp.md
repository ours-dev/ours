# kernel-smp outstanding repairs

## 1. The dbg image sometimes stops dead mid-boot, every CPU silent at once, with no panic and no error line

**Severity**
High: when it happens the machine stops for good, with no report. Intermittent: once in about a dozen dbg boots on 2026-10-07, not seen on rel.

**Symptom**
`scripts/kshell_run.py --out out/bringup.x86 pmnode.test ...` on the dbg image, at commit ad6c52f2's tree, timed out waiting for the shell's `--- end ---` with QEMU still running. The log, kept as `out/hangs/2026-10-07-kshell-dbg-fault-probe.log` in the project root (ignored by git), shows the following:

- All four CPUs stop printing within about 16 ms of each other, at guest time 9.986 s to 10.0026 s. The dbg image's per-thread debug lines (`I'm Thread 1`) stop too, so no CPU is running code that logs. A passing boot of the same build reaches `--- end ---` at about 12.4 s.
- No panic, no ASSERT, no error-level line.
- The last line of each CPU: cpu0 a user page fault, cpu1 an `IPI-Resched`, cpu2 and cpu3 their `I'm Thread` lines.
- The shell was in its fault-probe phase. It had just printed `probe gp_io killed` and was starting the next `fault_probe` process. The previous probe's address-space teardown had just freed frames through the buddy.

An earlier dbg timeout on code from before the buddy change, in the vma guard test, is weaker evidence: dbg runs that day finished between 5.7 s and 57 s against a 60 s limit, and that log was overwritten.

**Cause**
Unknown. Every CPU stopping within a few milliseconds, with logging stopped too, fits one CPU spinning with interrupts off on something the others then need: a lock held across an IPI wait, a TLB shootdown that waits for a CPU which never acknowledges, or a lock-order cycle between the zone lock, the address-space lock and the scheduler. The fault-probe phase tears down and creates address spaces back to back, so the IPI and TLB paths are the first suspects. That is a guess, not a finding.

**Change**
Reproduce, then look:

1. Loop dbg boots with QEMU's gdb stub on (`-s`, passed through `scripts/qemu_boot.py`), with a watchdog that notices when the log stops growing.
2. On a hang, attach lldb (`gdb-remote`) and take every vCPU's backtrace, RIP and RFLAGS.IF, plus the owner of each lock the backtraces wait on.
3. Fix the cause. Cap the loop and stop at the first hang: every boot is a minute of every core.

**Reference**
None yet.

**Acceptance**
The cause is named with every vCPU's backtrace from a hung boot, the fix is verified by a loop of dbg boots at least three times longer than the one that first reproduced the hang, with no hang, and `boot_check.py` passes.
