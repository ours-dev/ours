# kernel-time outstanding repairs

## 1. A deadline the device refuses at the end of an interrupt leaves a one-shot CPU with nothing armed

**Severity**
Medium: latent, the local APIC timer does not refuse a deadline; it bites when a device that can (the HPET) is used one-shot, or a driver returns an error.

**Symptom**
Latent, cannot be triggered today. At the end of `TimerQueue::run_expired_timers()`, `reprogram_locked()` asks the device for the next deadline. If the device refuses, the queue now records that it does not know what the device holds (`docs/changes/261010/kernel-time.md`, first entry) and counts the refusal, but nothing arms the device again. The CPU takes no interrupt until something activates, cancels or expires a timer on its queue. The tick timer, which would normally bring the CPU back, is itself waiting for the unarmed device, so a CPU with no other timer activity stops ticking until the lockup detector reports it.

**Cause**
Recording the refusal fixed the queue's belief about the device, not the device. The retry is left to the next queue event, which is not guaranteed to come. `ClockEventDevice::program_min_delta()` has already retried at growing deltas by the time a driver says no, so a refusal here means the device is not usable, not that it was a little late.

**Change**
Decide what a CPU should do when its device will not take a deadline. Options: fall back to arming a deadline of `now` plus the device's minimum delta, which cannot be refused for being too close; keep a bounded retry in the handler; or give the CPU back its periodic tick. This is the user's decision; the first is the smallest.

**Reference**
Linux `hrtimer_interrupt()` (`kernel/time/hrtimer.c`) never sees a refusal because it programs with `force=1`; its `retry:` loop is hang detection. None for a device that refuses even then.

**Acceptance**
A kernel test with a device that refuses its next deadline, once and then for good, finds the CPU still taking interrupts. Needs the test of item 1 in `docs/repairs/261009/kernel-time.md`.

## 2. The boot CPU has no tick between its local APIC enrolling and the Threading hook

**Severity**
Low: nothing reads a tick in that window.

**Symptom**
With `kernel.time.oneshot` on, the boot CPU's local APIC is enrolled at `InitLevel::Irq + 4` and its tick timer starts at `InitLevel::Threading`. Between them the HPET is detached and the queue is empty, so no interrupt arrives: jiffies does not advance and the lockup detector's heartbeat does not beat. Jiffies is then short by that window for the rest of the boot.

**Cause**
`TickSched::on_tick()` calls into the scheduler unless the work mode is Shutdown, and the work mode is the only record of whether the scheduler exists. So the tick timer cannot start before the mode leaves Shutdown, and the mode is the same field that says how the tick is produced (the second copy of the mode, `.agents/tasks/high-res-timers/DESIGN.md`, R1).

**Change**
Keep "the scheduler is ready" and "the tick is a timer" as two facts, so the tick timer can start when the device is enrolled and `on_tick()` skips only the scheduler. This is the removal of `WorkMode::Legacy` against `HpTick`, which R1 ends with.

**Reference**
None, this project's own design.

**Acceptance**
With the option on, `lockup status` shows the boot CPU's tick count within a tick of the other CPUs', and jiffies at the Threading hook is within a tick of the time since the local APIC was enrolled.

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

