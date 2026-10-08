# kernel-lib-logz4 outstanding repairs

## 1. On the dbg image the boot CPU loses 10% to 16% of its ticks, up to 44% in one second, probably waiting for the console with interrupts off

**Severity**
Medium: the boot CPU takes its tick, and so its timers and its time slices, up to hundreds of milliseconds late in a second, on the dbg image only, every boot. It is far from what the lockup detector reports (10 seconds in a row with no tick at all), so it is a latency problem, not a hang. The cause is a hypothesis, not a finding.

**Symptom**
Found by `lockup status` while measuring the lockup threshold (`docs/changes/261008/lockup-detector.md`, six boots on 2026-10-08):

- On dbg, cpu0 takes 10% to 16% fewer ticks over a boot than cpu1 to cpu3 (1307 against 1525 in 15 s of checks; 1344 and 1386 against 1507), and in its leanest second it took 72 to 76 of its 100 ticks (56 in one boot whose host was loaded by a second QEMU).
- cpu1, cpu2 and cpu3 never take fewer than 99 in a second.
- On rel no CPU takes fewer than 99, loaded or not.
- cpu0 prints none of dbg's `I'm Thread` lines: cpu2 and cpu3 print all of them, about 170,000 each in a 19 s boot.

A CPU loses a tick when it keeps interrupts off for more than one 10 ms period, because the local APIC holds one pending timer interrupt, not a count; or, under QEMU, when its vCPU is not run while the virtual clock goes on. rel, under the same QEMU, loses none, so it is something the dbg image does on cpu0.

**Cause**
Not established. The leading suspect is the console lock (`SerialLogger`, `kernel/lib/logz4/kernel.cpp`):

- `SerialLogger::write()` turns interrupts off, takes the lock, and polls COM1 for every byte of the line.
- The lock is a test-and-set with no queue (`acquire()`), so it is not fair: the CPU that lets it go can take it straight back.
- On dbg, cpu2 and cpu3 log a line each, back to back, for as long as they run the `Sample1` and `Sample2` threads (`kernel/top/main.cpp`). A third CPU that wants the console, cpu0 for its own info lines and the user page-fault lines, spins in `acquire()` with interrupts off until it happens to win, and every 10 ms it spins costs it a tick.

This would also explain why cpu2 and cpu3 lose nothing: each holds the lock for one line at a time and takes its tick between lines. Other suspects: the kernel shell reading COM1 on cpu0, and QEMU running cpu0's vCPU less because its port I/O to the UART takes QEMU's global lock.

**Change**
Measure before fixing; this is performance work and goes through the user (CLAUDE.md, "Working with the user"):

1. Confirm where cpu0's interrupts-off time goes: count, per CPU, the spins in `SerialLogger::acquire()` and the longest wait in TSC ticks, and print them beside `lockup status`. If the wait accounts for the lost ticks, the hypothesis holds.
2. Then the options, for the user to choose: a fair lock (a ticket lock) for the console, so that a waiting CPU is served in turn; keeping interrupts on while waiting for the lock and turning them off only once it is held; or a console that CPUs write into a buffer and one writer drains, which also stops a slow UART from holding any CPU with interrupts off. Each changes how lines from different CPUs interleave, and the panic path's bounded wait (`PANIC_LOCK_TICKS`) depends on the lock's shape.

**Reference**
Linux serialises console output through `console_lock()` and hands the printing of other CPUs' records to whichever CPU holds it (`console_unlock()` in `kernel/printk/printk.c`), and since the threaded printers each console is drained by a kthread of its own (`nbcon_kthread_func()`), so a CPU that logs does not wait for the UART. Zircon's kernel `dlog` writes into a ring (`DLog::Write()`), and a thread of its own drains the ring to the serial console (`DLog::DumperThread()`, `zircon/kernel/lib/debuglog/debuglog.cc:382`).

**Acceptance**
`lockup status` after a dbg boot shows cpu0's fewest ticks between two checks at 95 or more, and its heartbeat within 1% of the other CPUs' (allowing for its earlier start), with `boot_check.py` passing.
