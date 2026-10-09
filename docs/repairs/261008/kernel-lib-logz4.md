# kernel-lib-logz4 outstanding repairs

## 1. On the dbg image the boot CPU loses 10% to 16% of its ticks, up to 44% in one second, waiting for the console's lock with interrupts off while QEMU slows that CPU down

**Severity**
Medium: the boot CPU takes its tick, and so its timers and its time slices, up to hundreds of milliseconds late in a second, on the dbg image only, every boot. It is far from what the lockup detector reports (10 seconds in a row with no tick at all), so it is a latency problem, not a hang. Measured on 2026-10-08: the place and the mechanism are established (Cause); the fix is the user's choice (Change).

**Symptom**
Found by `lockup status` while measuring the lockup threshold (`docs/changes/261008/lockup-detector.md`, six boots on 2026-10-08):

- On dbg, cpu0 takes 10% to 16% fewer ticks over a boot than cpu1 to cpu3 (1307 against 1525 in 15 s of checks; 1344 and 1386 against 1507), and in its leanest second it took 72 to 76 of its 100 ticks (56 in one boot whose host was loaded by a second QEMU).
- cpu1, cpu2 and cpu3 never take fewer than 99 in a second.
- On rel no CPU takes fewer than 99, loaded or not.
- cpu0 prints none of dbg's `I'm Thread` lines: cpu2 and cpu3 print all of them, about 170,000 each in a 19 s boot.

A CPU loses a tick when it keeps interrupts off for more than one 10 ms period, because the local APIC holds one pending timer interrupt, not a count; or, under QEMU, when its vCPU is not run while the virtual clock goes on. rel, under the same QEMU, loses none, so it is something the dbg image does on cpu0.

Since 2026-10-08 `lockup status` also prints each CPU's account of its console writes (`docs/changes/261008/kernel-lib-logz4.md`), which shows this directly: on dbg as built, cpu0's line reads tens to hundreds of writes that kept interrupts off for longer than a tick.

**Cause**
Established on 2026-10-08 by four experiments, two dbg boots each (below). The objects:

- The tick, emulated periodic on a one-shot device: a handler that runs late arms the next deadline from the previous one and counts each deadline already past in jiffies only (`TickDevice::handle_periodic_event()`), so the CPU's heartbeat skips it. A stretch of L with interrupts off loses L / 10 ms - 1 ticks on average.
- `SerialLogger::write()`: interrupts off, a test-and-set lock with no queue (`acquire()`), one poll of COM1 per byte. Waiting for the lock is waiting with interrupts off.
- The boot CPU's LINT0: the firmware leaves it in ExtINT mode (`0x700`, read on this machine), the "virtual wire" that connects the legacy 8259 interrupt controller to the boot CPU alone. The kernel routes every ISA line through the IO-APIC, never uses the 8259, and never masks LINT0 (`XAPIC_CHIP` in `kernel/arch/x86/apic/local_apic.cpp`, the only code that would, is `[[maybe_unused]]`).
- QEMU's TCG: each vCPU is a host thread; device emulation runs under QEMU's one global lock.

The events:

1. cpu2 and cpu3 run `Sample1` and `Sample2` (`kernel/top/main.cpp`), about 16,000 lines a second each, two port accesses per byte.
2. Each write to the UART's transmit register makes QEMU's UART drive its interrupt line again (low: no UART interrupt is enabled), QEMU passes that to the 8259 whether or not it changed, and the 8259 hands its output to every local APIC that accepts 8259 interrupts: cpu0's alone, through LINT0. Done from cpu2's or cpu3's thread, that kicks cpu0's vCPU out of its translated code and makes it take the global lock to poll its APIC. This is the path in QEMU's source as known (`hw/char/serial.c`, `hw/intc/i8259.c`, `hw/intc/apic.c`), not re-read for the 11.1.1 in `prebuilt/`; experiment 4 establishes the effect.
3. cpu0 runs 20 to 60 times slower for as long as the debug threads print: the same user work that costs it 11 to 15 ticks costs 286 to 713 (experiment 1 against 2 and 4). User code takes no console lock, so this slowness is not the lock's.
4. In a test-and-set race the lock goes to whichever exchange lands first after the release. cpu1 waits 0.05 ms on average; cpu0, retrying at a fraction of the speed, waits 5 ms on average and up to 50 ms, for up to half of its time, with interrupts off.
5. Every wait over 10 ms costs cpu0 ticks.

Verdict on the hypothesis as first written:

- **Confirmed: the place.** cpu0's lost ticks go to `SerialLogger::write()` waiting for the lock with interrupts off: its longest interrupts-off stretch there is its longest wait to within 0.07 ms, 40 and 249 of its writes ran over a tick, and a fair lock alone removes the loss (experiment 3).
- **Refuted: unfairness alone as the reason it is cpu0.** cpu1 is the same third party to cpu2 and cpu3 and waits a hundred times less. The reason is QEMU slowing cpu0 (events 2 and 3); the unfair lock turns that slowness into interrupts-off time.
- **The other suspects.** QEMU running cpu0's vCPU less: confirmed, refined: not cpu0's own port I/O but the other CPUs', through LINT0 (experiment 4). The kernel shell reading COM1 on cpu0: ruled out, since it polls in experiment 4 too, where cpu0 loses nothing.

The measurements: `scripts/kshell_run.py --out out/bringup.x86 "lockup status"` on dbg, one boot at a time, host load average 3.6 to 6.9 on 15 cores. "Ticks lost" is cpu1's heartbeat minus cpu0's, plus 25: cpu0 starts ticking about 0.25 s before the APs and leads by 25 whenever nothing is lost (experiments 2 and 4, and the second boot of 3). "User-mode ticks" counts cpu0's `Exception[apic timer] from user space` trace lines. "Boot ends" is the guest time of the shell's `--- end ---`; the status follows it at once, so it covers the boot's ticks.

Experiment 1, as built: the test-and-set lock, the debug threads, LINT0 as the firmware left it.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 33.5 s | 11.8 s |
| Ticks taken, cpu0 and cpu1 | 2906 and 3111 | 859 and 892 |
| cpu0's ticks lost | about 230 | about 58 |
| cpu0's fewest ticks between two checks | 80 | 84 |
| cpu0's lines, and how many found the lock taken | 11,771 and 3,136 | 9,287 and 750 |
| cpu0's wait in all, and its longest | 16,095 ms and 49.570 ms | 3,683 ms and 21.443 ms |
| cpu0's writes off for over a tick, and its longest stretch | 249 and 49.643 ms | 40 and 21.479 ms |
| cpu1's waits, wait in all, and longest | 1,003, 50 ms and 0.437 ms | 3,024, 151 ms and 0.708 ms |
| Lines of cpu2 and of cpu3 | 484,467 and 484,344 | 139,349 and 139,466 |
| cpu0's user-mode ticks | 713 | 286 |

Experiment 2: the debug threads not started (a temporary early return in `labour_routine()`), the rest as built.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 4.3 s | 4.0 s |
| Ticks taken, cpu0 and cpu1 | 182 and 157 | 178 and 153 |
| cpu0's ticks lost | 0 | 0 |
| cpu0's lines, and how many found the lock taken | 11,051 and 6 | 11,056 and 5 |
| cpu0's wait in all, and its longest | 0.309 ms and 0.125 ms | 0.255 ms and 0.090 ms |
| cpu0's writes off for over a tick, and its longest stretch | 0 and 0.229 ms | 0 and 0.285 ms |
| cpu1's waits, wait in all, and longest | 55, 1.4 ms and 0.277 ms | 56, 1.4 ms and 0.178 ms |
| cpu0's user-mode ticks | 13 | 15 |

Experiment 3: a temporary ticket lock in place of the test-and-set lock (a ticket from `fetch_add`, served in order; panic mode by a compare-exchange try), debug threads and LINT0 as built. From here the account also gives the ticks its long stretches cost.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 17.5 s | 4.1 s |
| Ticks taken, cpu0 and cpu1 | 1508 and 1487 | 190 and 165 |
| cpu0's ticks lost | about 4 | 0 |
| cpu0's fewest ticks between two checks | 99 | no two checks yet |
| cpu0's lines, and how many found the lock taken | 11,444 and 2,737 | 17,737 and 6,557 |
| cpu0's wait in all, and its longest | 161 ms and 0.300 ms | 203 ms and 0.231 ms |
| cpu0's writes off for over a tick, longest stretch, ticks they cost | 0, 0.856 ms, 0 | 0, 0.424 ms, 0 |
| cpu1's waits, wait in all, and longest | 949, 49 ms and 0.321 ms | 2,954, 81 ms and 0.285 ms |
| Lines of cpu2 and of cpu3 | 234,806 and 234,811 | 487 and 38,467 |
| cpu0's user-mode ticks | 338 | 0: the shell's user work ran on cpu1 and cpu2 |

Experiment 4: LINT0 masked on the boot CPU (a temporary mask in `init_apic_lint1()`), the test-and-set lock and the debug threads as built.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 4.4 s | 4.5 s |
| Ticks taken, cpu0 and cpu1 | 199 and 174 | 201 and 176 |
| cpu0's ticks lost | 0 | 0 |
| cpu0's lines, and how many found the lock taken | 11,049 and 2,528 | 11,037 and 2,495 |
| cpu0's wait in all, and its longest | 91 ms and 0.568 ms | 91 ms and 0.461 ms |
| cpu0's writes off for over a tick, longest stretch, ticks they cost | 0, 0.592 ms, 0 | 0, 0.485 ms, 0 |
| cpu1's waits, wait in all, and longest | 995, 40 ms and 0.297 ms | 1,008, 44 ms and 0.605 ms |
| Lines of cpu2 and of cpu3 | 36,464 and 36,622 | 36,278 and 36,306 |
| cpu0's user-mode ticks | 11 | 11 |

Two limits. Experiments 2 to 4 watched under 2 s of ticks, against 9 s and 31 s in experiment 1, because the boot ends sooner once the problem is gone; the evidence there is per write: cpu0 wrote about 11,000 lines under as much contention (cpu2 and cpu3 wrote faster, not slower) and none waited more than 0.6 ms, where experiment 1's 9,000 to 12,000 lines had 40 to 249 over 10 ms. And experiment 1 has no "ticks they cost" (the counter came after it): its 40 stretches of at most 21.5 ms can account for at most 80 lost ticks against about 58 measured, which fits, but cannot be checked one by one.

**Change**
Not fixed: the choice is the user's. The options, ranked by the data; each is independent of the others, and the first two can be combined.

1. **Recommended first: mask LINT0 on the boot CPU** once the IO-APIC carries the ISA lines (`kernel/arch/x86/apic/local_apic.cpp`, beside `init_apic_lint1()`). Mechanism: with LINT0 masked, the boot CPU no longer accepts the 8259's output, so QEMU has no reason to poll or kick it when a UART line is driven; the kernel uses nothing that comes through the 8259. Data: experiment 4, every measure back to the no-debug-threads level, the boot's time included. Cost: one register write at boot, and none at run time. What it does not do: make the lock fair. It removes the one condition measured to starve a CPU here, not the possibility. It is an x86-apic change, and it would also settle `docs/repairs/261005/kernel-top.md` item 1 (two boots measured; that item's acceptance asks for five). Not measured: whether the 8259 should also be masked at its own registers, which the kernel does not do either; it is not what stops the kick.
2. **A fair console lock, a ticket lock.** Mechanism: a waiting CPU takes a ticket and is served in order, so its wait is bounded by the lines queued ahead of it, at most one per other CPU. Data: experiment 3, cpu0's longest wait 0.30 ms and no lost ticks, but cpu0 still 25 times slower (338 user-mode ticks) and the boot as spread as before (17.5 s). Cost: two 32-bit counters for one flag; the handoff is strict, so a holder or a queued waiter whose vCPU the host deschedules holds up every CPU behind it, where test-and-set lets the next live CPU in; and the panic path's bounded wait needs a try by compare-exchange, since a ticket once taken cannot be handed back (experiment 3's code did this). Lines from different CPUs come out in arrival order rather than in race order.
3. **Interrupts on while waiting, off only once the lock is held.** Mechanism: spin with the caller's interrupt state, then turn interrupts off and try once more, looping on failure. Data: in experiment 1 the wait was nearly all of the interrupts-off stretch (49.570 of 49.643 ms), so the lost ticks would go, but cpu0 would still wait 41% to 52% of its time for the console: the latency moves from the tick to the logging thread. Cost: a few lines; the lock stays unfair and its wait unbounded. Not measured.
4. **A buffered console:** CPUs write into a ring and one thread drains it to COM1. Mechanism: no CPU waits for the UART or for another CPU's line; only the drainer polls COM1. Cost: a kernel thread, a policy for a full ring (the debug threads write 30,000 lines a second, far more than a real 115200-baud port carries), a synchronous path for panic that bypasses the drainer, and lines that reach the wire after the event. It does not stop the slowdown: the drainer's port writes still kick cpu0 while LINT0 is unmasked. The long-term shape of a console, and the largest change. Not measured.
5. **Quiet the debug threads** (`docs/repairs/261005/kernel-top.md` item 1, Change). Data: experiment 2, everything gone. Cost: their role as the first sign of preemptive round-robin, which is the user's call; and the console's two problems stay, only unexercised.

**Reference**
- The console: Linux serialises console output through `console_lock()` and hands the printing of other CPUs' records to whichever CPU holds it (`console_unlock()` in `kernel/printk/printk.c`), and since the threaded printers each console is drained by a kthread of its own (`nbcon_kthread_func()`), so a CPU that logs does not wait for the UART. Zircon's kernel `dlog` writes into a ring (`DLog::Write()`), and a thread of its own drains the ring to the serial console (`DLog::DumperThread()`, `zircon/kernel/lib/debuglog/debuglog.cc:382`).
- LINT0: Linux masks LVT0 once interrupts go through the IO-APIC, `apic_write(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_EXTINT)` in `check_timer()` (`arch/x86/kernel/apic/io_apic.c`), and turns ExtINT back on only if the timer works through the 8259 alone. Zircon's checkout masks the 8259 (`pic_disable()` in `platform_init_apic()`, `zircon/kernel/platform/pc/interrupts.rs:111`) and was not found to touch LINT0.
- Ticket locks: Linux's x86 spin lock was a ticket lock from 2.6.25 until queued spin locks replaced it in 4.2 (`queued_spin_lock_slowpath()`, `kernel/locking/qspinlock.c`), whose MCS queue gives the same order without every waiter spinning on one cache line.

**Acceptance**
`lockup status` after a dbg boot shows cpu0's fewest ticks between two checks at 95 or more, its heartbeat within 1% of the other CPUs' (allowing for its earlier start), and its console line at 0 writes off for longer than a tick, with `boot_check.py` passing. Because a fixed boot ends within about 5 s, read the status once the debug threads have run for at least 15 s, so that it covers many checks; `kshell_run.py` types it at the end marker and has no way to wait first today, so this needs one.
