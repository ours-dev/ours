# kernel-time outstanding repairs

## 4. The emulator delivers a timer that is not already due on a grid of about 1.25 ms, and the kernel's own interrupt work is a few microseconds

**Severity**
Low: a property of the emulator and the host, and a few microseconds of the kernel's work that could be saved; not a defect.

**Symptom**
With `kernel.time.oneshot` on, a 1 ms timer fires a median of 0.28 to 0.38 ms late, and a periodic timer's interrupts, once the hold-off asks for a deadline in the future, come 1.2 ms apart whatever the callback costs (`docs/changes/261021/kernel-time.md`, the entries on the hold-off and on `timershare.test big`). Measured 2026-10-22 (`docs/architecture/timer/oneshot-interrupt-cost.md`): a timer armed for a deadline already past is delivered in about 30 µs; one armed for 25 µs to 1 ms comes at 1.28 ms, for 2 ms at 2.53 ms, for 3 ms at 3.78 ms.

**Cause**
Two separate things.

The delivery grid is the emulator's or the host's: the HPET comparator behaves the same as the local APIC timer, 2 and 4 virtual CPUs give the same table, and running QEMU at the highest `taskpolicy` latency tier does not change it. What it is on the host is not established; it looks like a 25 % slack on a timeout, possibly macOS coalescing timers. The kernel cannot change it.

The kernel's own work in an interrupt outside the callback is 6 to 9 µs (the earlier figure of about 110 µs was a remainder of derived quantities and does not survive): 3 to 5 clock reads at about 0.8 µs each (the earlier 7 to 10 µs for a read was taken under logging load and was not reproduced), 3 to 5 APIC register writes at 0.4 to 1 µs, and locks. A periodic timer that restarted inside the pass made the device be armed twice, the first arming overwritten at once by the second; that is fixed (`docs/changes/261023/kernel-time.md`), and a storm's handler is about 4 µs shorter for it.

**Change**
Performance work, so it goes through the user. The candidates, each with the saving measured and the cost, are the table in `docs/architecture/timer/oneshot-interrupt-cost.md`: handing the end-of-pass reading to `program_event()` (about 1 µs), skipping an unchanged divisor write (about 0.5 µs), a TSC clock source (3 to 5 µs, and a decision of its own), x2APIC (no evidence). The first candidate, not arming the device inside the pass, was chosen and done on 2026-10-23: one clock read and two APIC writes less in the storm, about 4 µs off a handler of 35. The rest together would take 2 to 5 µs more and change nothing a timer's user sees on this host. Ruled out for now: doing any of it before the user chooses.

**Reference**
Zircon on x86 prefers the TSC as its clock and reads it with `rdtsc`, falling back to the HPET or the PIT (`zircon/kernel/platform/pc/timer.cc`, `platform_current_raw_ticks_synchronized()`), so a clock read on its timer path is not a memory-mapped access.

**Acceptance**
The user's choice among the candidates, and for each one chosen the same stamps (`exp/irq-cost`) showing the saving.

## 6. A CPU whose every tick device has failed has no tick

**Severity**
Medium by effect, deferred by decision (the user, 2026-10-21): not to be done now. Latent, since no device in the tree refuses, and the lockup detector reports the CPU after the threshold.

**Symptom**
Latent, cannot be triggered today. When a CPU's tick device refuses a deadline, `TickDevice::on_device_refused()` moves the tick to the best device that has not failed (`docs/changes/261011/kernel-time.md`). If none is left it says so as an error and returns; the CPU keeps running threads but takes no timer interrupt, so nothing preempts a thread that does not block and no timer on it fires.

**Cause**
Every fallback today is another Oneshot device. Nothing takes the CPU back to a Periodic one, which would be coarse (timers at the tick) but alive.

**Change**
Leave Oneshot: cancel the tick timer, put a device that can run Periodic into service with the periodic handler, set the mode back. The queue then runs from `on_tick()` again, since that now follows the device's mode. The usable device is the one the legacy line delivers, on one CPU at a time without a group. Ruled out for now: doing it without a device that can fail to test it on (`tickreplace.test` has the refusing fake).

**Reference**
None to follow. Linux has no way back either: a CPU in one-shot mode refuses a periodic-only device (`tick_check_preferred()`, `kernel/time/tick-common.c`), `tick_setup_device()` keeps the mode across a replacement, `hrtimer_switch_to_hres()` (`kernel/time/hrtimer.c`) has no inverse, and the one place that sets the mode back to periodic is `tick_shutdown()`, the CPU going offline. It covers the gap by the broadcast device, which this tree has, and does not cover a CPU left with no usable device. Doing this would be this project's own design, and a departure from "a CPU enters one-shot and never leaves".

**Acceptance**
`tickreplace.test` with every real device failed ends with the CPU ticking at HZ and its 1 ms timers firing at tick granularity.

## 8. `timer.test`'s hold-off check fails in about one run in four

**Severity**
Low: a test that is wrong about when it can be right; the hold-off itself is not shown to be wrong.

**Symptom**
`timer.test` ends `FAILED -- 1 of 12 checks`, the line `and it came no sooner than about the pass's own length after the pass`, with `the next interrupt came 20 us later at the soonest` (or 1527 µs, where the bound is 1800 µs). Measured 2026-10-23 on the dbg image, one-shot: 1 run of 6 on the untouched baseline, 4 of 10 on the build of `docs/changes/261023/kernel-time.md`; the samples are too small to say the two rates differ. The other checks of the command and its other cases pass in those runs.

**Cause**
Not established. The likely one: the check wants every trial's B to run only after the hold-off, but the preemption deadline is deliberately not held off (`TimerQueue::earliest_deadline_locked()` applies the hold-off to the timers and `std::min`s the preemption deadline after it). If the command's thread has its slice end in the gap between A's pass and B's deadline (a trial spans about 4 ms; the slice length was not checked), that interrupt's pass also runs B, 20 µs after A's pass. A gap of about 20 µs in both runs, on both builds, fits an interrupt that was already due. Not verified: no run counted whether the pass that ran B was a slice end.

**Change**
First find out whether it is the slice end: count, per trial, whether `TimerQueue`'s preemption path ran in the pass that ran B (`TIMER_QUEUE_PREEMPTS` before and after is enough). If it is, the check should discard such a trial, as it discards a trial where B ran in A's pass, instead of failing; and the check on `counted != 0` stays, so that a build with no valid trial still fails. Ruled out: loosening the bound, which would let a missing hold-off through.

**Reference**
None, this project's own test.

**Acceptance**
`timer.test` passes 20 runs in a row on the dbg image, one-shot, and still fails when the hold-off is taken out of `earliest_deadline_locked()` (a mutation run).
