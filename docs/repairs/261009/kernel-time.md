# kernel-time outstanding repairs

## 1. TimerQueue's path for a device that refuses a deadline has no test

**Severity**
Low: the behaviour is fixed (`docs/changes/261010/kernel-time.md`), but nothing runs it, so a later edit can break it unseen.

**Symptom**
Latent, cannot be triggered today. `TimerQueue::reprogram_locked()` forgets the armed deadline when `TickDevice::program_event()` fails, so the next reprogram tries again. Every CPU's tick device runs Periodic, where `program_event()` always returns Ok, so no run ever reaches the refusal.

**Cause**
A refusal needs a CPU in one-shot mode and a device whose programming fails once. Neither exists yet, and binding a fake device to a running CPU's `TickDevice` would take that CPU's tick away.

**Change**
Once a CPU can run one-shot (`.agents/tasks/high-res-timers/DESIGN.md`), add a kernel test that gives that CPU's `TickDevice` a device which refuses its first `set_next_event_tick()`, arms a timer, arms a second timer with the same earliest deadline, and sees the first fire. It must fail against the code before 2026-10-10.

**Reference**
None, this project's own design.

**Acceptance**
The test passes, fails when `reprogram_locked()` records the deadline unconditionally again, and both images boot.

## 2. Two owners program the same tick device: SchedClock::reset_preempt() bypasses TimerQueue

**Severity**
Medium: latent, its only caller is Fair under `sched_nohz`, which is off (`kernel/kernel_params.gni`).

**Symptom**
Latent, cannot be triggered today. With the device in one-shot mode, `SchedClock::reset_preempt()` (`kernel/kernel/task/sched/sched_clock.cpp:44-55`) programs `TickDevice::Current` directly. It can push the device past a nearer timer's deadline, and `TimerQueue::m_next_deadline` then names a deadline the device is no longer armed for, so that timer is late until something else reprograms.

**Cause**
The per-CPU event device has two writers that do not know about each other: the timer queue, which caches what it armed, and the scheduler's preemption deadline. Whoever writes last wins.

**Change**
Make the timer queue the only writer: keep the preemption deadline as a slot inside `TimerQueue` and arm the device for the earlier of that slot and the queue's head, keeping `SchedClock::reset_preempt()`'s interface so `fair.*` is untouched. Depends on the one-shot work in `.agents/tasks/high-res-timers/DESIGN.md`.

**Reference**
Zircon `TimerQueue::PreemptReset()` and `UpdatePlatformTimerLocked()` (`zircon/kernel/kernel/timer.cc`): the slice end is a field of the timer queue, and the platform timer is armed for the earliest of it and the timer lists.

**Acceptance**
With one-shot mode and `sched_nohz` on, a timer due before the next preemption still fires on time (a kernel test logging both deadlines); both images boot.

## 3. TickDevice::switch_to_oneshot() commits the mode before it knows the switch worked

**Severity**
Medium: latent, nothing calls it yet; the first caller inherits it.

**Symptom**
Latent, cannot be triggered today. `kernel/kernel/time/tick_device.cpp:256-261` sets `m_mode = Mode::Oneshot` and then returns `setup_oneshot()`'s status. A failed switch leaves the tick device claiming one-shot over a device that may still be periodic, so `program_event()` takes the one-shot path against it. It also never checks `Feature::Oneshot`, and nothing asserts that interrupts are off while the handler is swapped (the same class as `docs/repairs/260926/kernel-time.md` item 1).

**Cause**
Written as a skeleton with no caller; the error path was never exercised.

**Change**
Check the device feature first, swap the handler and the mode with interrupts off, and restore both on failure.

**Reference**
Linux `tick_switch_to_oneshot()` (`kernel/time/tick-oneshot.c`) checks for a one-shot-capable device and returns -EINVAL before touching the mode, and runs with interrupts off.

**Acceptance**
A kernel test that asks a periodic-only device to switch gets an error and finds the mode unchanged.

## 4. Timer::forward() does not move a timer whose deadline is exactly now

**Severity**
Low: one extra expiry pass at most, for a periodic timer that fires exactly on its deadline.

**Symptom**
`Timer::forward()` (`kernel/kernel/time/timer.cpp:27-30`) returns 0 when `expires >= now`, so a timer forwarded at its own deadline keeps that deadline and runs again in the same expiry pass. Under one-shot mode the emulated tick (`TickSched` HpTick) and the sampler's timers forward this way on every fire.

**Cause**
The guard treats "due now" as "not yet due".

**Change**
Return 0 only when `expires > now`, so a timer at its deadline moves one period.

**Reference**
Linux `hrtimer_forward()` (`kernel/time/hrtimer.c`) returns 0 only when `now` is before the expiry.

**Acceptance**
A host or kernel test forwarding a timer at exactly its deadline gets an overrun of 1 and a deadline one interval later.
