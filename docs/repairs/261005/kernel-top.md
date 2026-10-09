# kernel-top outstanding repairs

## 1. On dbg, work placed on CPU 0 sometimes runs fifty to a hundred times slower, with the debug threads printing

**Severity**
Medium: no result is wrong, but how long the dbg boot's shell checks take swings from about 29 to 45 s between runs, and a check that does a fixed amount of work under a time-based measurement (`profile`) was starved to one sample short of failing (fixed on its side, `docs/changes/261005/prof.md`, 2026-10-05).

**Symptom**
`boot_check.py --out out/sampler` on dbg, three runs on 2026-10-05 whose logs were kept, host load average about 11 on 15 cores with another session's QEMU running:

- The profile check (10,000 `vmo_create`/`handle_close` pairs and 20 million additions in user code, under a sampling session) took 0.6 s in one run and 19.4 s and 13.3 s in the other two. Its samples show the slow runs on CPU 0 and the fast one on CPU 1, each with its CPU to itself: the other CPUs held the two debug threads and an idle thread.
- In the slow runs the kernel samples of the workload are spread over the ordinary system-call path (`MemCache::free`, `MemCache::NodeCache::take_object`, `ProcessDispatcher::current`) with no hotspot, so the code did not wait on anything: every instruction was slower.
- The other CPUs did not slow down: the debug threads' log rate stayed at about 17,000 lines a second throughout.
- Which checks are slow changes per run: one run had only profile slow; another had process_create, thread_create, handle transfer, port observer limit, handle quota and sampler each one to five seconds slow, and profile fast.

**Cause**
Not known. The debug threads `Sample1` and `Sample2` (`kernel/top/main.cpp`, the `gibberish` lambda) print `I'm Thread N` in a loop with no sleep, so two CPUs write COM1 about 650,000 times a second between them, each byte a port read and a port write. The leading hypothesis is that this costs CPU 0 alone under QEMU's TCG: each port write goes through QEMU's serial device, which may kick the vCPU that ISA interrupts are routed to (CPU 0) out of its translated code even though the guest never enables a UART interrupt. That would slow every instruction on CPU 0 and nothing on CPU 1, as observed. Unconfirmed; the serial logger's lock was ruled out for the kernel part, because the slow samples do not sit at `SerialLogger::write()`.

Measured on 2026-10-08 (`docs/repairs/261008/kernel-lib-logz4.md` item 1, four experiments of two dbg boots each): the leading hypothesis holds, and the path is named. The firmware leaves the boot CPU's LINT0 in ExtINT mode (`0x700`), the "virtual wire" from the legacy 8259 to CPU 0 alone, and the kernel never masks it. Every UART register write by another CPU makes QEMU drive the UART's interrupt line again, pass it through the 8259, and poll CPU 0's local APIC from the writing CPU's thread, which kicks CPU 0's vCPU and makes it take QEMU's global lock. Counted by CPU 0's `Exception[apic timer] from user space` trace lines, its ticks in user code, the same boot's user work cost CPU 0 11 to 15 ticks without the debug threads or with LINT0 masked, and 286 to 713 with both as built: 20 to 60 times slower, as this item saw. Masking LINT0 on the boot CPU alone (a temporary change) brought it back to 11 with the debug threads printing.

One correction to the reasoning above: a sample that does not sit at `SerialLogger::write()` does not rule the lock out, because the sampler is driven by an interrupt and `write()` keeps interrupts off, so the sampler cannot see time spent there at all. The lock is ruled out for this item on other grounds: user code takes no console lock and was slowed as much.

The dbg boot's time spread follows from the same cause: no repair item tracks it, but `docs/changes/261007/lockup-detector.md` notes dbg boots of 4.8 s to 59.2 s against `boot_check.py`'s 60 s, and `docs/repairs/261007/kernel-smp.md` item 1 of 5.7 s to 57 s. The boot's time depends on how much of the shell's user work lands on CPU 0 (end marker, guest time): 33.5 s and 11.8 s as built, with 713 and 286 of CPU 0's ticks in user code; 17.5 s under a fair console lock, which does not remove the slowdown, with 338; 4.1 s under the same lock in a boot whose user work ran on CPU 1 and CPU 2; and 4.0 to 4.5 s in every boot without the debug threads or with LINT0 masked.

**Change**
Measure first. Boot dbg a few times with the debug threads removed and compare each check's duration with the runs above; then, to separate the CPU from the threads, boot with them kept and the shell's work pinned to CPU 0 and then to CPU 1. If the threads are the cause, the fix is to stop them flooding the log: remove them, or make each sleep between lines. Their output was the first sign of preemptive round-robin (`docs/changes/260903/scheduler.md`); whether something should keep that role is the user's call. Since 2026-10-08 there is a third way, which keeps the threads as they are: mask LINT0 on the boot CPU once the IO-APIC carries the ISA lines, so that QEMU no longer interrupts CPU 0 for another CPU's UART writes (`docs/repairs/261008/kernel-lib-logz4.md` item 1, Change, option 1). Two boots measured; the acceptance below asks for five.

**Reference**
None, this project's own debug code.

**Acceptance**
Five dbg boots in a row each finish the shell's checks within a few seconds of one another, and each prints `profile: ... 0 dropped`.
