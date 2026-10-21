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

## 5. A periodic timer whose callback is slower than its period starves the thread it interrupts, and nothing limits it

**Severity**
Medium: shown under QEMU TCG with slow callbacks; whether a timer in the tree has one that slow is not known. The sampler's 10 µs timer with a callback that unwinds a kernel stack is the candidate.

**Symptom**
`timershare.test big` (2026-10-21): with a 10 µs periodic timer, the thread keeps about 24 % of its CPU at a callback of 0.5 ms, 11 % at 1 ms, 4 % at 2 ms and 4 % at 4 ms (`docs/architecture/timer/callback-cpu-share.md`, last section). The share is $g_{\text{eff}} / (C + g_{\text{eff}})$ with the gap the emulated timer leaves, 110 to 180 µs. With callbacks of the sampler's size (20 to 100 µs) the thread keeps 64 % to 100 %, so the case does not show at that size here.

**Cause**
After a pass the next deadline is the first multiple of $P$ after the callback ended, which is within $P$ of now. The device is armed for it and interrupts again as soon as it can; nothing makes the gap depend on how long the callback took. On real hardware the gap is a few microseconds, so a callback of tens of microseconds would starve the thread the way a callback of milliseconds does here.

**Change**
Decided by the user; the candidates are in `.agents/tasks/high-res-timers/STATUS.md` (a delay for the whole device after a long pass, as Linux does; or a delay for only the timer whose callback was slow, so that the others are not made late; or none). Whichever is chosen, check it with `timershare.test big`. Ruled out for now: writing the guard without a decision on which.

**Reference**
Linux `hrtimer_interrupt()` (`kernel/time/hrtimer.c`): after three retries a still-due timer marks a hang and the device is armed `min(100 ms, max_hang_time)` ahead.

**Acceptance**
`timershare.test big` with the guard, $\lambda = 1$: every row at 50 % or above of the CPU; and `timer.test` and the default `timershare.test` unchanged.

## 6. A CPU whose every tick device has failed has no tick

**Severity**
Medium: latent, since no device in the tree refuses, and the lockup detector reports the CPU after the threshold.

**Symptom**
Latent, cannot be triggered today. When a CPU's tick device refuses a deadline, `TickDevice::on_device_refused()` moves the tick to the best device that has not failed (`docs/changes/261011/kernel-time.md`). If none is left it says so as an error and returns; the CPU keeps running threads but takes no timer interrupt, so nothing preempts a thread that does not block and no timer on it fires.

**Cause**
Every fallback today is another Oneshot device. Nothing takes the CPU back to a Periodic one, which would be coarse (timers at the tick) but alive.

**Change**
Leave Oneshot: cancel the tick timer, put a device that can run Periodic into service with the periodic handler, set the mode back. The queue then runs from `on_tick()` again, since that now follows the device's mode. The usable device is the one the legacy line delivers, on one CPU at a time without a group. Ruled out for now: doing it without a device that can fail to test it on (`tickreplace.test` has the refusing fake).

**Reference**
None, this project's own design.

**Acceptance**
`tickreplace.test` with every real device failed ends with the CPU ticking at HZ and its 1 ms timers firing at tick granularity.
