# kernel-lib-logz4 changes

## Each CPU's console writes are counted and `lockup status` prints them, and four experiments find where the boot CPU's ticks go (2026-10-08)

**Files**
- `kernel/lib/logz4/kernel.cpp` — `WriteAccount`, one per CPU (`CPU_LOCAL WRITE_ACCOUNT`): the writes, the writes that found the lock taken, their total and longest wait, the writes that kept interrupts off for longer than a tick, the longest such stretch, and how far past one period those stretches ran in all; `SerialLogger::write()` and `acquire()` fill it; `ours::log_console_waits()` prints one line per online CPU.
- `kernel/include/ours/start.hpp` — declares `log_console_waits()` beside the console's other glue (`hold_console_lock_forever()`).
- `kernel/arch/x86/apic/local_apic.cpp`, `kernel/arch/x86/kernel/include/ours/arch/apic.hpp` — `apic_tsc_hz()`, the TSC rate the APIC calibration already measures, so that the counts can be given in milliseconds.
- `kernel/commands/lockup.cpp` — `lockup status` prints the console accounts before the checker's notes.
- `kernel/lib/lockup_detector/README.md` — one paragraph on those lines.
- `docs/repairs/261008/kernel-lib-logz4.md` — item 1: the measurements, the cause, the options re-ranked.
- `docs/repairs/261005/kernel-top.md` — item 1: what the measurements say about the boot CPU's slowness and the dbg boot's time.

**Symptom**
On the dbg image the boot CPU took 10% to 16% fewer ticks than the others (`docs/repairs/261008/kernel-lib-logz4.md` item 1). Nothing in the kernel could say where its interrupts-off time went: the console lock was the leading suspect, but it kept no count of anything.

**Cause**
This change is instrumentation; what it found is the cause of that item, and is recorded there in full. In short, as objects and then events:

- The tick: emulated periodic on a one-shot device. Its handler arms the next deadline from the previous one and, when that is already past, counts the missed period in jiffies only (`TickDevice::handle_periodic_event()`), so the CPU's heartbeat does not move for it. A stretch of L with interrupts off therefore loses L / period - 1 ticks on average.
- `SerialLogger::write()`: interrupts off, then a test-and-set lock with no queue, then one poll of COM1 per byte. A CPU that waits for the lock waits with interrupts off.
- QEMU's local APIC for cpu0: the firmware leaves its LINT0 in ExtINT mode (`0x700`), the "virtual wire" that connects the legacy 8259 interrupt controller to the boot CPU, and the kernel never masks it, though it routes every ISA line through the IO-APIC.

1. cpu2 and cpu3 run the debug threads, which write about 16,000 lines a second each, two port accesses per byte.
2. Each write to the UART's transmit register makes QEMU's UART drive its interrupt line again (low: the kernel enables no UART interrupt), and QEMU passes that on to the 8259 whether or not it changed; the 8259 re-evaluates its output and QEMU hands it to every local APIC that accepts 8259 interrupts. Only cpu0's does, through LINT0. Done from cpu2's or cpu3's thread, that hand-off kicks cpu0's vCPU out of its translated code and makes it take QEMU's global lock to poll its APIC. This is QEMU's code path as known from its source (`hw/char/serial.c`, `hw/intc/i8259.c`, `hw/intc/apic.c`), not re-read for the 11.1.1 in `prebuilt/`; experiment 4 below is what establishes the effect.
3. So cpu0 runs 20 to 60 times slower than the others for as long as the debug threads print: user work that takes 11 to 15 of its ticks without them took 286 to 713 with them.
4. In a test-and-set race the lock goes to whichever CPU's exchange lands first after the release. cpu2 and cpu3 retry at full speed; cpu0 retries rarely, so it almost never wins: its average wait was 5 ms against cpu1's 0.05 ms, the longest 50 ms, half of its time in one boot, all with interrupts off.
5. Every wait longer than a period costs cpu0 ticks: 249 such writes in one boot, 40 in another.

The lock's unfairness turns cpu0's slowness into interrupts-off time; QEMU's kick is why it is cpu0. Removing either removes the lost ticks (experiments 3 and 4); only removing the kick also removes the slowness.

**Change**
The account is per CPU and written only by its own CPU inside `write()`, with interrupts off, so each field moves on with a relaxed load and store rather than a locked read-modify-write; the fields are atomics only so that `lockup status` can read them from another CPU. A write that takes the lock at its first try touches none of the waiting fields:

```cpp
// before
auto acquire() -> bool {
    if (!logz4::in_panic_mode()) {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
        return true;
    }
```

```cpp
// after
auto acquire(WriteAccount *const account) -> bool {
    if (!logz4::in_panic_mode()) {
        if (!m_lock.test_and_set(std::memory_order_acquire)) {
            return true;
        }
        auto const start = arch::Tick::get().tsc;
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
        if (account != nullptr) {
            account->add_wait(arch::Tick::get().tsc - start);
        }
        return true;
    }
```

`write()` also reads the TSC once after it turns interrupts off and once before it turns them back on, and gives the difference to `add_write()`, which keeps the longest and, for a stretch longer than one period, counts it and adds up how far past the period it ran. `log_console_waits()` divides that sum by the period: the ticks the console cost the CPU. There is no account before logz4's CPU hook is installed (`InitLevel::CpuLocal`), since the per-CPU storage does not exist yet; only the boot CPU runs then, so nothing waits.

A line of `lockup status` after a dbg boot:

```text
console: cpu0 wrote 11049 lines; 2528 found the lock taken and waited 91.274 ms in all, the longest 0.568 ms; 0 kept interrupts off for longer than a tick, the longest 0.592 ms, which cost about 0 ticks
```

Choices:

- **Kept, not dropped.** The waiting fields cost nothing on an uncontended write. The per-write fields cost two TSC reads and a few relaxed stores per line, against two port accesses per byte (about 120 for a 60-byte line, each a VM exit under QEMU and about a microsecond on real hardware). In return the console's share of any CPU's lost ticks can be read after any boot, and whichever fix item 1 gets is verified by them: its acceptance is cpu0 at "0 kept interrupts off for longer than a tick".
- **Inside `lockup status`, not a `lockup console` subcommand.** `scripts/kshell_run.py` ends a command that is not a test at the shell's prompt, which on dbg is seldom whole among the debug threads' bytes, while it ends `lockup status` at its last line. Printed first, the console lines change nothing there.
- **The interrupts-off stretch, not the hold.** The stretch is what loses ticks, and it is the wait plus the hold; the data shows the hold is the small part (cpu0's longest stretch 49.643 ms, its longest wait 49.570 ms).
- **The TSC rate from the APIC calibration.** It already measures the TSC against the clock source and kept the result private; `apic_tsc_hz()` returns it, 0 before the calibration, when the status says it cannot give times. A second measurement in the command was rejected: two numbers for one fact.

**Reference**
- Zircon keeps lock contention as kernel counters, a sum and a maximum: `ChainLockTransaction::UpdateContentionCounters()` adds to `chainlock.contention_ns` and raises `chainlock.contention_ns_max` (`zircon/kernel/lib/kconcurrent/chainlock_transaction.cc:24`), read with `k counters`. Linux's `CONFIG_LOCK_STAT` keeps per lock class the contentions and the total and longest wait and hold (`lock_contended()`, `lock_acquired()` in `kernel/locking/lockdep.c`, shown in `/proc/lock_stat`). Here the account is per CPU rather than per lock because the question is which CPU loses ticks, and there is one console lock.
- The LINT0 mechanism: Linux masks LVT0 once interrupts go through the IO-APIC, `apic_write(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_EXTINT)` in `check_timer()` (`arch/x86/kernel/apic/io_apic.c`), and unmasks ExtINT only if the timer works through the 8259 alone. Zircon's checkout masks the 8259 (`pic_disable()` in `platform_init_apic()`, `zircon/kernel/platform/pc/interrupts.rs:111`) and was not found to touch LINT0.

**Verification**
- `cpp-write-and-verify`: `scripts/format_cpp.py` clean on every changed file; `clang-tidy -p out/bringup.x86` on `kernel/lib/logz4/kernel.cpp`, `kernel/commands/lockup.cpp` and `kernel/arch/x86/apic/local_apic.cpp` reports nothing in them (the headers' findings predate this change).
- `boot_check.py` on the committed tree: dbg PASS (shell finished at 5.6 s, 35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (shell finished at 3.9 s, 33 passed, 9 skipped, 0 failed, 6/6 required), host-tests PASS (143).
- The measurements: four experiments, two dbg boots each, every one `scripts/kshell_run.py --out out/bringup.x86 "lockup status"`, one boot at a time, host load average 3.6 to 6.9 on 15 cores. Each experiment's table is in `docs/repairs/261008/kernel-lib-logz4.md` item 1; the summary, for cpu0:

| Experiment | Boot ends | Ticks lost | Longest wait | Writes off for over a tick | Its user-mode ticks |
|---|---|---|---|---|---|
| 1. as built | 33.5 s, 11.8 s | about 230, 58 | 49.6 ms, 21.4 ms | 249, 40 | 713, 286 |
| 2. no debug threads | 4.3 s, 4.0 s | 0, 0 | 0.13 ms, 0.09 ms | 0, 0 | 13, 15 |
| 3. ticket lock | 17.5 s, 4.1 s | about 4, 0 | 0.30 ms, 0.23 ms | 0, 0 | 338, 0 |
| 4. LINT0 masked | 4.4 s, 4.5 s | 0, 0 | 0.57 ms, 0.46 ms | 0, 0 | 11, 11 |

"Ticks lost" is cpu1's heartbeat minus cpu0's, plus the 25 ticks cpu0 starts ahead (it leads by 25 whenever nothing is lost: experiments 2 and 4, and 3's second boot). "User-mode ticks" counts cpu0's `Exception[apic timer] from user space` trace lines, its ticks that landed in user code: the same boot's user work on cpu0 takes 11 to 15 of them when nothing slows cpu0. In 3's second boot the shell's user work ran on cpu1 and cpu2, not cpu0.

- 10 boots in all: 8 for the experiments, 2 for `boot_check.py` (dbg and rel side by side).

**Deliberately not done**
- No fix. Which one is the user's call (`docs/repairs/261008/kernel-lib-logz4.md` item 1, Change). The ticket lock and the LINT0 mask were temporary, built and booted from the worktree, and not committed.
- Experiments 2 to 4 watched cpu0 for under 2 s of ticks, against 9 s and 31 s in experiment 1, because without the problem the boot ends sooner and the status follows it. The evidence there is per write, not per second: cpu0 wrote about 11,000 lines in each, under as much contention from cpu2 and cpu3, and not one waited more than 0.6 ms, where experiment 1's same 9,000 to 12,000 lines had 40 to 249 that waited more than 10 ms. A long-run check is the acceptance of whichever fix is chosen.
- QEMU's internal path (event 2 above) was not read in the 11.1.1 source nor traced; the experiment shows the effect, the source as known gives the mechanism.
- No histogram of waits, and no separate hold time: the questions were answered without them.
- `scripts/kshell_run.py` was not changed: the console lines come before the status's last line.

**How it was found**
- Experiment 1, counters in, `kshell_run.py --out out/bringup.x86 "lockup status"`: cpu0's longest wait matched its longest interrupts-off stretch to 0.07 ms, so the stretch is the wait; it waited 5 ms on average where cpu1, also a third party to cpu2 and cpu3, waited 0.05 ms. A fair-lock argument cannot tell those two CPUs apart, so something made cpu0 itself slow.
- The trace lines already in the log measured that: cpu0's `Exception[apic timer] from user space` count, its ticks in user code, which takes no console lock, was 20 to 60 times what the same work takes without the debug threads (experiment 2). Code that does not touch the lock ran slower, so the slowness is not the lock's.
- `local APIC: the firmware left LINT1 at 0x400` in the log, and no code in the kernel that touches LINT0 (`XAPIC_CHIP`, the only user, is `[[maybe_unused]]`): the boot CPU alone was left on the virtual wire. A temporary log line in experiment 4 read LINT0 as `0x700`, ExtINT unmasked, before masking it.
- The two temporary fixes then separated the layers: the ticket lock removed the waits and the lost ticks but not the slowness (experiment 3, 338 user-mode ticks); masking LINT0 removed all three (experiment 4).

**The user's prediction**
None: the user chose not to write one for this measurement (2026-10-08).

**Three questions you should be able to answer**
1. Why does a CPU that keeps interrupts off for 25 ms lose one or two ticks rather than none, and why would a CPU that is merely slow, with interrupts on, lose none?
2. cpu0 and cpu1 are both third parties to cpu2 and cpu3 on the console lock. Why did cpu0 wait a hundred times longer, and what does that tell you about where a test-and-set lock's unfairness bites?
3. Why does a fair lock fix the lost ticks here but leave the dbg boot's time as spread as before, while masking LINT0 fixes both?
