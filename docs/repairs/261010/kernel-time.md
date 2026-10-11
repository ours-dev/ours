# kernel-time outstanding repairs

## 3. The fallback to the periodic tick, when a device will not go one-shot, has no test

**Severity**
Low: it is a few lines and the local APIC always accepts.

**Symptom**
Latent, cannot be triggered today. `TickDevice::rebind_device()` calls `setup_oneshot_tick()`; if the device refuses Oneshot, it logs the status and runs `setup_periodic()` on the same device, with the mode still Periodic. No run reaches that branch.

**Cause**
It needs a device that claims `Feature::Oneshot` and fails `switch_mode(Oneshot)`, and a CPU whose binding can be redone without losing its tick.

**Change**
A kernel test that enrols a fake device on a spare CPU's `TickDevice` before that CPU runs (or a host test of the selection with a fake `ClockEventDevice`), checks the mode is Periodic afterwards and that the device is in Periodic or emulated-periodic service.

**Reference**
Linux `tick_switch_to_oneshot()` (`kernel/time/tick-oneshot.c`) returns -EINVAL for a device that cannot, leaving the mode alone.

**Acceptance**
The test fails if `setup_oneshot_tick()` commits `m_mode` before the device accepted Oneshot.

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

## 5. Whether a periodic timer slower than its period needs a guard is not known, because TCG cannot produce the case

**Severity**
Medium: unknown. On real hardware a 10 µs sampler timer with a callback that unwinds a kernel stack could interrupt a CPU back to back; the formula in `docs/architecture/timer/callback-cpu-share.md` says the thread keeps $P / (C + P)$ of its CPU at most.

**Symptom**
Latent, cannot be triggered under QEMU TCG. `timershare.test` finds the thread keeping 45 % to 54 % of its CPU with a 10 µs timer and callbacks of 63 to 150 µs, because the emulated device cannot deliver faster than about every 360 µs.

**Cause**
The guard was to cover a case in which the next interrupt arrives within the gap `forward()` leaves. That needs a device that delivers in microseconds and a clock that is cheap to read, neither of which this emulator has.

**Change**
When a machine with a fast timer is available (real hardware, or a TSC-deadline guest under KVM), run `timershare.test` there. If the thread's share is near the formula's, add the guard: after a pass, do not arm the device sooner than $\lambda$ times the time the pass took, with $\lambda = 1$ to start. Ruled out for now: writing the guard without being able to see it work.

**Reference**
Linux `hrtimer_interrupt()` (`kernel/time/hrtimer.c`): after three retries a still-due timer marks a hang and the device is armed `min(100 ms, max_hang_time)` ahead.

**Acceptance**
`timershare.test` on that machine, with the guard, keeps the thread at or above $\lambda / (1 + \lambda)$ of its CPU; without it, the numbers are recorded and the item is closed as not needed there.

## 6. A CPU whose every tick device has failed has no tick

**Severity**
Medium: latent, since no device in the tree refuses, and the lockup detector reports the CPU after the threshold.

**Symptom**
Latent, cannot be triggered today. When a CPU's tick device refuses a deadline, `TickDevice::device_refused()` moves the tick to the best device that has not failed (`docs/changes/261011/kernel-time.md`). If none is left it says so as an error and returns; the CPU keeps running threads but takes no timer interrupt, so nothing preempts a thread that does not block and no timer on it fires.

**Cause**
Every fallback today is another Oneshot device. Nothing takes the CPU back to a Periodic one, which would be coarse (timers at the tick) but alive.

**Change**
Leave Oneshot: cancel the tick timer, put a device that can run Periodic into service with the periodic handler, set the mode back. The queue then runs from `on_tick()` again, since that now follows the device's mode. The usable device is the one the legacy line delivers, on one CPU at a time without a group. Ruled out for now: doing it without a device that can fail to test it on (`tickreplace.test` has the refusing fake).

**Reference**
None, this project's own design.

**Acceptance**
`tickreplace.test` with every real device failed ends with the CPU ticking at HZ and its 1 ms timers firing at tick granularity.

## 7. A broadcast device that refuses inside its interrupt handler is reported to no member

**Severity**
Medium: latent, the HPET is the only group device and has not been seen to refuse.

**Symptom**
Latent, cannot be triggered today. `TickBroadcast::on_device_event()` re-arms the device for the members that are left; if it is refused, the refusal is counted and the device's armed deadline forgotten, but no member's queue is told, since the call did not come from one. Each member finds out at its next deadline, when `set_deadline()` is refused in turn and `device_refused()` moves that member; until then none of them has an interrupt.

**Cause**
The group programs the device on its own behalf in the handler, outside any member's `reprogram_locked()`, so the refusal has no queue to travel through.

**Change**
Mark the device failed and send each member an IPI (a new `IpiEvent`, or `IpiEvent::Timer` with the member then reprogramming) so that every member's queue runs `notify_refusal()` on its own CPU. Ruled out: calling `device_refused()` for a member from the handler's CPU, which would swap another CPU's `TickDevice`.

**Reference**
None, this project's own design.

**Acceptance**
A test that makes a group's device refuse once the group has two members finds both members on another device within a tick.

