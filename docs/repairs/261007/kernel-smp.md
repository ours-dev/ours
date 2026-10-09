# kernel-smp outstanding repairs

## 1. The dbg image sometimes stops dead mid-boot, every CPU silent at once, with no panic and no error line

**Severity**
High: when it happens the machine stops for good, with no report. Intermittent: once in about a dozen dbg boots on 2026-10-07, then not in 30 more boots of the same image (see Change); not seen on rel.

**Symptom**
`scripts/kshell_run.py --out out/bringup.x86 pmnode.test ...` on the dbg image, at commit ad6c52f2's tree, timed out waiting for the shell's `--- end ---` with QEMU still running. The log, kept as `out/hangs/2026-10-07-kshell-dbg-fault-probe.log` in the project root (ignored by git), shows the following:

- All four CPUs stop printing within about 16 ms of each other, at guest time 9.986 s to 10.0026 s. The dbg image's per-thread debug lines (`I'm Thread 1`) stop too, so no CPU is running code that logs. A passing boot of the same build reaches `--- end ---` at about 12.4 s.
- No panic, no ASSERT, no error-level line.
- The last line of each CPU: cpu0 a user page fault, cpu1 an `IPI-Resched`, cpu2 and cpu3 their `I'm Thread` lines.
- The CPUs do not stop together, and cpu3 stops first. It had been printing `I'm Thread 2` from the Sample2 busy loop about every 60 µs, and prints its last at 9.986358 s. After that cpu0 still finishes two user page faults (9.991545 s and 9.993391 s), cpu1 takes an `IPI-Resched` (9.991544 s), and cpu2 prints `I'm Thread 1` for 16 ms more, until 10.002628 s, longer than one period of the 100 Hz tick. A CPU in that loop stops printing only when it stops running the loop: it is stuck in an interrupt handler, or it switched to something that never gives the CPU back. So the others stopped one by one, each as it reached whatever cpu3 held or was waiting for.
- The shell was in its fault-probe phase. It had just printed `probe gp_io killed` and was starting the next `fault_probe` process. The previous probe's address-space teardown had just freed frames through the buddy.

An earlier dbg timeout on code from before the buddy change, in the vma guard test, is weaker evidence: dbg runs that day finished between 5.7 s and 57 s against a 60 s limit, and that log was overwritten.

**Cause**
Unknown. Every CPU stopping within a few milliseconds, with logging stopped too, fits one CPU spinning with interrupts off on something the others then need: a lock held across an IPI wait, a TLB shootdown that waits for a CPU which never acknowledges, or a lock-order cycle between the zone lock, the address-space lock and the scheduler. The fault-probe phase tears down and creates address spaces back to back, so the IPI and TLB paths are the first suspects. That is a guess, not a finding. The order in which the CPUs went quiet (Symptom) says to start from what cpu3 was doing at 9.986 s.

Read and ruled out for today, so that the next attempt need not read it again: `Scheduler::switch_context()` takes the next thread's PI lock while it holds its own run-queue lock, the reverse of the order the tree states (PI lock outside, run-queue lock inside). It closes a cycle only if some path holds a Ready thread's PI lock and then takes that thread's run-queue lock. None does today: the one that would, `Scheduler::Pi::common()`, is called from nowhere, because the PI-propagation entry points are empty stubs. It will matter once they are filled in.

Also ruled out, on 2026-10-09: the `interruptline.test` lockup on dbg reported that day is a different failure. There, one CPU took a #UD in `WaitQueue::unpark_thread()` and printed the error line, the other CPUs kept ticking, and the lockup detector reported the stopped CPU 10 s later (`docs/repairs/261009/x86-kernel.md` item 1). The #UD came from an uncommitted edit to `Thread::Current::exit()` in the shared tree, and did not reproduce on committed code in 8 boots. This hang printed no error line and stopped every CPU, and its image was built in a worktree of its own.

**Change**
Reproduce, then look:

1. Loop dbg boots with QEMU's gdb stub on (`-s`, passed through `scripts/qemu_boot.py`), with a watchdog that notices when the log stops growing.
2. On a hang, attach lldb (`gdb-remote`) and take every vCPU's backtrace, RIP and RFLAGS.IF, plus the owner of each lock the backtraces wait on.
3. Fix the cause. Cap the loop and stop at the first hang: every boot is a minute of every core.

Attempt on 2026-10-07, on branch `fix/smp-hang`: not reproduced in 30 boots.

- Image: the hung boot's own bytes, `kernel.dev.dbg_1.obi` with sha256 f9a8cfface2af405 (`qemu_boot.py` prints it), built at 0e7e637b, whose kernel code is ad6c52f2's. Four CPUs and 512 MiB, the `qemu_boot.py` defaults, one boot at a time.
- Driver (`out/hunt/hunt.py` in that worktree, ignored by git, with the lldb attach script `out/hunt/inspect.py`): boots through `scripts/qemu_boot.py` with `-gdb tcp::<port>` and `-monitor tcp:127.0.0.1:<port>,server=on,wait=off` added, both idle unless a boot hangs. After `--- end ---` it types `pmnode.test`, `pmzone.test`, `vmosnapshot.test` and `vmaundo.test` and requires each one's `: ok` line. No new serial byte for 15 s, at any point before the last verdict, counts as a hang: QEMU is kept and the monitor's `info registers -a` saved, for lldb to attach.
- Result: 30 passes, in 16 s to 63 s of wall time each. Every boot reached `--- end ---`, so its shell ran the whole fault-probe phase, and every one passed the four tests. The logs of the last four are kept beside the driver; each has 19 `fault_probe:` lines and `launcher checks passed`.

What the next attempt could change, most promising first:

- (a) Recommended: make a hang report itself. A lockup watchdog that notices a CPU whose tick count has stopped and has every CPU print its backtrace, through an NMI for the stuck one, would catch the first hang of any boot, with no debugger attached at the right moment. It is a new kernel facility (the `Monitored` option of `sync::SpinLock` is reserved for its lock half), so it needs the user's decision. The user took it on 2026-10-07 (`.agents/tasks/lockup-watchdog`). Since its step 1, a boot that has gone silent can be dumped by sending `nmi` to QEMU's monitor: every CPU's thread, registers and backtrace, printed once, then a panic (`docs/changes/261007/lockup-detector.md`). Since its step 2, a CPU that stops taking its tick while another CPU still ticks is found and reported by itself, after 10 s (`kernel.lockup.threshold`); a hang in which every CPU has interrupts off still needs the monitor's `nmi`. Since its step 3, `boot_check.py` and `kshell_run.py` send that `nmi` themselves after 15 s of silence, before the end marker (and on dbg, in `kshell_run.py`, for the whole run), and keep the report in the log. So the hunt of step 1 above no longer needs a debugger or a driver of its own: it is a capped loop of `kshell_run.py --out <dir> pmnode.test pmzone.test vmosnapshot.test vmaundo.test`, stopping at the first run that fails, and the first hang reports every CPU's thread, registers and backtrace by itself (the ours-verify skill, "Reading a lockup report").
- (b) Loop under host load. The hung boot may have shared the host with other boots (`boot_check.py` boots dbg and rel side by side, and other sessions boot too); a vCPU descheduled by the host at the wrong moment widens a race window. Running two boots at once needs the user's leave.
- (c) `SMP=8`, for more cross-CPU interleavings per boot.

The watchdog of (a) is in, as of 2026-10-08: all four steps of `.agents/tasks/lockup-watchdog` landed (its `RESULT.md`), on both images, on by default. This item stays open until a hang is caught and its cause named. No boot has hung since (step 4 alone booted dbg 5 times and rel 3 times). The next hang reports itself in the log of whatever `boot_check.py` or `kshell_run.py` run it happens in, in one of two ways:

- Some CPU still ticks. After 10 s, the CPU that finds the stuck one prints `lockup: cpu<N> took no tick for 10 s, found by cpu<M>`, then each CPU's count of checks in a row without a tick (the larger, the earlier it went quiet), then one block per CPU: thread, pc as an image offset, interrupts on or off, spin locks held, ticks taken, registers and backtrace, the stuck CPU's last. Then the panic `lockup: cpu<N> took no tick for 10 s`, and its crash record.
- Every CPU has interrupts off, so none ticks. The console falls silent; after 15 s the script sends QEMU's monitor `nmi`, cpu0 prints `lockup: dump asked for by an NMI on cpu0; each CPU's state follows` and the same blocks, and the verdict says `console silent for 15s before the end, so QEMU was sent nmi: lockup report, ...`. Here the ticks taken say in what order the CPUs stopped: one that went quiet earlier is behind by about a hundred a second.

Either way the report is printed once, whole, and kept in the log; the ours-verify skill says how to read it ("Reading a lockup report"), and Change step 2 above becomes reading the report instead of attaching lldb. The 10 s rests on data: in six boots, alone and with a second QEMU on the host, no CPU went even one check without a tick (`docs/changes/261008/lockup-detector.md`). `lockup status` on dbg shows that margin after any boot.

**Reference**
None yet.

**Acceptance**
The cause is named with every vCPU's backtrace from a hung boot, the fix is verified by a loop of dbg boots at least three times longer than the one that first reproduced the hang, with no hang, and `boot_check.py` passes.
