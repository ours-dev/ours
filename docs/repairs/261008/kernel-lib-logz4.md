# kernel-lib-logz4 outstanding repairs

## 2. The console's lock is an unfair test-and-set lock waited on with interrupts off, so a CPU slower than the others can wait for it for many ticks

**Severity**
Low: nothing reaches it measurably today. Since every CPU masks LINT0 (2026-10-09, `docs/changes/261009/x86-apic.md`), no CPU on dbg has waited longer than 2.9 ms for the console in 16 s of the debug threads writing flat out, and none has lost a tick. The lock itself bounds no wait, so a CPU that is slowed again, for any reason, loses ticks again. Item 1 of this file, the boot CPU losing ticks while QEMU slowed it, was fixed by that change; this is what remains of it, numbered 2 because item 1 is cited by the records that measured it.

**Symptom**
Latent, cannot be triggered today. It is reached when one CPU retries the console lock much more slowly than the others that contend for it. It was reached on dbg until 2026-10-09: QEMU slowed the boot CPU 20 to 60 times through its unmasked LINT0, and that CPU waited up to 50 ms for the lock with interrupts off, in 249 writes of one boot for longer than a tick, about 230 ticks lost (experiment 1 of 2026-10-08; its table is in `docs/changes/261009/x86-apic.md`, Cause).

Today, `scripts/kshell_run.py --out out/bringup.x86 --delay 15 "lockup status"` on dbg shows the lock heavily contended but no wait near a tick: cpu2 and cpu3, which run the debug threads, each find it taken on 98% of their writes and wait about 7.5 s in all over the run, the longest 2.9 ms; cpu0 and cpu1 wait at most 0.6 ms.

**Cause**
The objects:

- `SerialLogger::write()` (`kernel/lib/logz4/kernel.cpp`): turns interrupts off, takes the lock in `acquire()`, polls COM1 once per byte, releases.
- The lock: one `test_and_set` flag with no queue. A release lets in whichever CPU's next exchange lands first.
- The tick: a CPU that keeps interrupts off for longer than one period loses ticks, because its local APIC holds one pending timer interrupt, not a count.

The events, when one CPU is slower:

1. The fast CPUs retry the exchange at full speed; the slow one retries at a fraction of that rate.
2. At each release a fast CPU's exchange almost always lands first, so the slow CPU loses race after race; its wait has no bound but the others' pauses.
3. It waits with interrupts off, so every wait over a period loses its ticks.

What can make a CPU slower than its rivals on real hardware or another emulator: a sibling hyperthread busy on the same core, a lower clock, a vCPU the host runs less. The 2026-10-08 measurements showed that the unfairness alone is not enough (cpu1, the same third party as cpu0, waited a hundred times less), and that the slowness alone is not enough either: it is the two together.

**Change**
Not decided; the choice is the user's, and is performance work, to be laid out and measured first. To measure, recreate a slow CPU, for instance by leaving LINT0 open again in a worktree, which reproduces 2026-10-08's experiment 1. The options, from that item, each independent of the others:

1. **A fair lock, a ticket lock.** A waiting CPU takes a ticket and is served in order, so its wait is bounded by the lines queued ahead of it, at most one per other CPU. Data: experiment 3 of 2026-10-08, with the boot CPU still slowed: its longest wait 0.30 ms and no lost ticks. Cost: two 32-bit counters for one flag; the handoff is strict, so a holder or queued waiter whose vCPU the host deschedules holds up every CPU behind it, where test-and-set lets the next live CPU in; the panic path's bounded wait needs a try by compare-exchange, since a ticket once taken cannot be handed back. Lines from different CPUs come out in arrival order rather than race order.
2. **Interrupts on while waiting, off only once the lock is held.** Spin with the caller's interrupt state, then turn interrupts off and try once more, looping on failure. In experiment 1 the wait was nearly all of the interrupts-off stretch (49.570 of 49.643 ms), so the lost ticks would go; the lock stays unfair and the wait unbounded, moved from the tick to the logging thread. A few lines. Not measured.
3. **A buffered console.** CPUs write into a ring and one thread drains it to COM1, so no CPU waits for the UART or for another CPU's line. Cost: a kernel thread, a policy for a full ring (the debug threads write about 48,000 lines a second between them, far more than a real 115200-baud port carries), a synchronous path for panic that bypasses the drainer, and lines that reach the wire after the event. The long-term shape of a console, and the largest change. Not measured.

**Reference**
- The console: Linux serialises console output through `console_lock()` and hands the printing of other CPUs' records to whichever CPU holds it (`console_unlock()` in `kernel/printk/printk.c`), and since the threaded printers each console is drained by a kthread of its own (`nbcon_kthread_func()`), so a CPU that logs does not wait for the UART. Zircon's kernel `dlog` writes into a ring (`DLog::Write()`), and a thread of its own drains the ring to the serial console (`DLog::DumperThread()`, `zircon/kernel/lib/debuglog/debuglog.cc:382`).
- Ticket locks: Linux's x86 spin lock was a ticket lock from 2.6.25 until queued spin locks replaced it in 4.2 (`queued_spin_lock_slowpath()`, `kernel/locking/qspinlock.c`), whose MCS queue gives the same order without every waiter spinning on one cache line.

**Acceptance**
With a CPU slowed as in 2026-10-08's experiment 1 (LINT0 left open in a worktree, the debug threads printing), `lockup status` read after at least 15 s of the debug threads (`kshell_run.py --delay 15`) shows that CPU's console line at 0 writes off for longer than a tick and its longest wait under a period, and its fewest ticks between two checks at 95 or more; and on the tree as built, `boot_check.py` passes.
