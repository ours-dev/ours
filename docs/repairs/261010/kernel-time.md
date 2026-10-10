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
