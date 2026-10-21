# kernel-time outstanding repairs

## 4. A one-shot interrupt costs about 110 µs outside its callback under TCG, and the device delivers about 200 µs after it is armed

**Severity**
Low: a performance observation on the emulator, not a defect.

**Symptom**
With `kernel.time.oneshot` on, a 10 µs periodic timer is interrupted about every 360 to 510 µs, whatever its callback costs, and a 1 ms timer fires a median of about 0.9 ms late (`docs/changes/261010/kernel-time.md`, the entries on the one-shot entry and on `timershare.test`). The thread being interrupted keeps about half its CPU even when the callback is ten times the period.

**Cause**
Not established. Measured: a clock-source read costs 7 to 10 µs; the callback takes about 40 µs more than the work it is asked for, which is `forward()` and the queue's bookkeeping. Derived from the spacing, the callback and the thread's share, not timed separately: about 110 µs of interrupt work outside the callback, and about 200 µs between the device being armed and the interrupt arriving, the second of which is probably QEMU delivering the local APIC timer. Each interrupt reads the clock source at least three times (the start of `run_expired_timers()`, `forward()`, `ClockEventDevice::program_event()`), and acknowledges the local APIC with an MMIO write.

**Change**
Performance work, so it goes through the user first (CLAUDE.md): measure where the 110 µs goes (clock reads, the EOI write, the lock round trips, the interrupt entry) before choosing among candidates such as passing one reading of `now` down the path instead of reading three, or a TSC clock source (`docs/repairs/260903/x86-clock.md` item 3). Ruled out for now: doing any of it without a measurement.

**Reference**
Zircon on x86 prefers the TSC as its clock and reads it with `rdtsc`, falling back to the HPET or the PIT (`zircon/kernel/platform/pc/timer.cc`, `platform_current_raw_ticks_synchronized()`), so a clock read on its timer path is not a memory-mapped access.

**Acceptance**
A table of where one interrupt's time goes, and for each candidate the change it makes, agreed with the user before any of it is done.

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
