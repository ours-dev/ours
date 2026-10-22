# kernel-time outstanding repairs

## 4. The emulator delivers a timer that is not already due on a grid of about 1.25 ms, and the kernel's own interrupt work is a few microseconds

**Severity**
Low: a property of the emulator and the host, and a few microseconds of the kernel's work that could be saved; not a defect.

**Symptom**
With `kernel.time.oneshot` on, a 1 ms timer fires a median of 0.28 to 0.38 ms late, and a periodic timer's interrupts, once the hold-off asks for a deadline in the future, come 1.2 ms apart whatever the callback costs (`docs/changes/261021/kernel-time.md`, the entries on the hold-off and on `timershare.test big`). Measured 2026-10-22 (`docs/architecture/timer/oneshot-interrupt-cost.md`): a timer armed for a deadline already past is delivered in about 30 µs; one armed for 25 µs to 1 ms comes at 1.28 ms, for 2 ms at 2.53 ms, for 3 ms at 3.78 ms.

**Cause**
Two separate things.

The delivery grid is the emulator's or the host's: the HPET comparator behaves the same as the local APIC timer, 2 and 4 virtual CPUs give the same table, and running QEMU at the highest `taskpolicy` latency tier does not change it. What it is on the host is not established; it looks like a 25 % slack on a timeout, possibly macOS coalescing timers. The kernel cannot change it.

The kernel's own work in an interrupt outside the callback is 6 to 9 µs (the earlier figure of about 110 µs was a remainder of derived quantities and does not survive): 3 to 5 clock reads at about 0.8 µs each (the earlier 7 to 10 µs for a read was taken under logging load and was not reproduced), 3 to 5 APIC register writes at 0.4 to 1 µs, and locks. A periodic timer that restarts inside the pass makes the device be armed twice, the first arming overwritten at once by the second.

**Change**
Performance work, so it goes through the user. The candidates, each with the saving measured and the cost, are the table in `docs/architecture/timer/oneshot-interrupt-cost.md`: not arming the device inside the pass (about 2 to 3 µs in the storm, one clock read and two APIC writes), handing the end-of-pass reading to `program_event()` (about 1 µs), skipping an unchanged divisor write (about 0.5 µs), a TSC clock source (3 to 5 µs, and a decision of its own), x2APIC (no evidence). All of them together take 3 to 7 µs from a handler of 6 to 9 µs and change nothing a timer's user sees on this host. Ruled out for now: doing any of it before the user chooses.

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
