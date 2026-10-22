# Where one one-shot timer interrupt spends its time

Measured on 2026-10-22 under QEMU TCG on the macOS host, with the stamps and the `irqcost.test` command of the throwaway branch `exp/irq-cost` (commit `e1228282`, never merged), on top of baseline `65ed6bf6`. It answers `docs/repairs/261010/kernel-time.md` item 4, whose figures (about 110 µs of interrupt work outside the callback and about 200 µs between arming and the interrupt) had been derived and not timed.

## Method

TSC stamps (`arch::Tick::get().tsc`) are taken along the path of one local APIC timer interrupt, kept per CPU in a ring of 512, and read back by a console command: at the entry of the handler and after its EOI; at the entry of `run_expired_timers()`, after its first clock read, after the callbacks, after its end-of-pass clock read, and after `reprogram_locked()`; in `program_event()` around its clock read; and around the two APIC register writes of `set_next_event_tick()`. Counters give the HPET reads and the APIC register writes of each interrupt, and the stamp taken when the initial count is written gives the delivery latency: the time from arming to the handler's entry, less the delay that was asked for.

The TSC of this emulator runs at 1 GHz and advances in steps of 1000 ticks, which is 1 µs: two adjacent stamps differ by 0 or 1 µs. Medians of single stages are therefore good to about a microsecond, and the means, which average the rounding out over hundreds of interrupts, are given beside them. The outliers of the host (a stage that took hundreds of microseconds once) pull the means above the medians.

Workloads, on the last CPU, the dbg image, a one-shot build: **storm**, a 10 µs periodic timer with a 20 µs callback on a pinned thread for a second, 512 interrupts, one after another; **single**, one 1 ms timer at a time, 300 of them, as `timer.test` arms them; **sweep**, a timer armed for X µs, the time to its callback, 40 times for each X. An idle CPU was tried and took two interrupts in two seconds (it is tickless), too few to say anything.

## The handler is small

Microseconds, per interrupt, from the entry of the handler to its return:

| Stage | storm, median | storm, mean | single, median | single, mean |
|---|---|---|---|---|
| EOI write | 1 | 0.8 | 1 | 0.9 |
| dispatch to `run_expired_timers()` | 0 | 0.3 | 0 | 0.4 |
| first clock read | 1 | 1.4 | 1 | 1.7 |
| the callbacks | 26 | 27.1 | 2 | 1.8 |
| end-of-pass clock read | 1 | 1.9 | 1 | 0.9 |
| `program_event()` clock read | 1 | 1.0 | 1 | 0.5 |
| the two APIC writes of arming | 1 | 2.1 | 0 | 0.8 |
| the rest (to `program_event()`, the arming's setup, after the writes, the tail: locks and bookkeeping) | 0 | 0.9 | 2 | 1.3 |
| **whole handler** | **33** | **36.0** | **8** | **8.8** |
| **whole handler less the callbacks** | **6** | **8.9** | **6** | **7.0** |

Per interrupt: **5 HPET reads and 5 APIC register writes in the storm**, 3 and 3 in the single case. One `SteadyClock::now()` costs **0.8 µs** (the mean of 2000 in a loop on an otherwise idle machine); the item's earlier figure of 7 to 10 µs was taken in `timershare.test` with the other CPUs logging, and was not reproduced here.

So the kernel's own work in an interrupt outside the callback is 6 to 9 µs, not 110, and the clock reads are about half of it.

## The storm arms the device twice

The storm needs 5 reads and 5 writes against the single case's 3 and 3, because a periodic timer that restarts inside the pass is enqueued with `enqueue_locked()`, becomes the head, and `reprogram_locked()` arms the device for it, and then the end of the pass arms it again, for the same timer and now with the hold-off. The first arming, one clock read and two APIC writes, is overwritten at once. Nothing else can arm this CPU's queue while it is inside its own interrupt.

## Delivery: the emulator's grid

Armed for X µs, the callback came after (microseconds, 40 repeats; median, smallest, largest):

| Armed for | Came after |
|---|---|
| 0 µs (already due) | 29 (21 to 868) |
| 5 µs | 26 (19 to 867) |
| 25 µs | 1282 (863 to 1292) |
| 100 µs | 1281 (851 to 1293) |
| 300 µs | 1282 (847 to 1294) |
| 600 µs | 1280 (846 to 1290) |
| 1000 µs | 1279 (1273 to 2120) |
| 2000 µs | 2534 (2101 to 3417) |
| 3000 µs | 3780 (3355 to 4625) |

A deadline that is already past, or so near that it is past by the time it is armed, is delivered in about 30 µs. Any other deadline is delivered about 1.25 times as late as it was asked for, in steps: everything from 25 µs to 1 ms comes at 1.28 ms, 2 ms at 2.53 ms, 3 ms at 3.78 ms. The `single` workload's 274 µs of delivery latency for a 998 µs request is the same grid (1.27 ms in all).

This is not the kernel and not the local APIC model:
- the same 1 ms timers through the HPET comparator, as `tickbcast.test` runs them, come a median of 320 to 330 µs late, as they do through the local APIC;
- 2 and 4 virtual CPUs give the same table;
- running QEMU at the highest throughput and latency tier of `taskpolicy` (`-t 0 -l 0`) gives the same table.

What it is on the host was not established. It looks like a 25 % slack on the timeout of the main loop's poll, which would be macOS coalescing timers. It also explains the lateness of every short timer measured here: a 1 ms timer is late by the grid's 0.28 ms, whatever the kernel does.

## Reading the old figures again

- The spacing of a storm without the hold-off was 190 to 200 µs per callback. The sweep shows two regimes, about 30 µs for a deadline already past and 1.28 ms for any other, so a mean of about 190 µs is a mixture, roughly 87 % immediate deliveries and 13 % on the grid, and not a device that delivers after 200 µs. This is consistent with the data and was not tested.
- With the hold-off (2026-10-21) the next deadline is in the future by the length of the pass, so every interrupt is on the grid. That is the 1.2 ms of extra gap between callbacks in `timershare.test big`, and the reason its shares are above the guard's half. On a device that delivers when asked the gap is the hold-off.
- The 110 µs "outside the callback" was a remainder: the spacing, less the callback, less the 200 µs, each of them derived. It does not survive. The handler outside the callback is 6 to 9 µs, and the rest of the spacing is the delivery.

## What this means for the candidates of the item

Savings are per interrupt, from the table above.

| Candidate | Mechanism | Saves | Cost and risk |
|---|---|---|---|
| Do not arm the device inside the pass | While `run_expired_timers()` runs, a restart's `enqueue_locked()` leaves the programming to the pass's end | one clock read and two APIC writes in the storm, about 2 to 3 µs of its 6 to 9 | small; nothing else arms this queue during its own interrupt |
| Hand the end-of-pass reading to `program_event()` | One reading serves the hold-off and the arming | one clock read, about 1 µs | an optional `now` through three signatures; the reading is a few microseconds old |
| Skip the divisor write when unchanged | A per-CPU cached divisor | one APIC write, about 0.5 µs | low; invalidated where the timer is set up |
| A TSC clock source | `rdtsc` for every read | all 3 to 5 reads, about 3 to 5 µs | calibration, synchronisation, and the policy of trusting the TSC (TCG does not report it invariant); a decision of its own |
| x2APIC writes | MSR instead of MMIO | under a microsecond | large; no evidence it matters |

All of them together would take 3 to 7 µs from a handler of 6 to 9 µs, and none would change what a timer's user sees on this host, where delivery is 1.25 ms. On a machine whose timer interrupts when asked, the clock reads (an HPET read is a device access of the order of a microsecond) are the part that counts, and the TSC clock source is the one that removes them.

## Limits

TCG's TSC steps of 1 µs; a single run on one host; the cause of the grid is not known; the instrumentation itself costs less than the TSC's step; no real hardware.
