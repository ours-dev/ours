# kernel-time outstanding repairs

## 1. TimerQueue::reprogram_locked() caches a deadline the device refused, so a one-shot CPU can lose every later timer

**Severity**
Medium: latent while every tick device runs Periodic, where programming is a no-op; it bites the moment a CPU runs one-shot.

**Symptom**
Latent, cannot be triggered today. In one-shot mode, a `program_event()` that fails leaves the device unarmed while `m_next_deadline` already says it is armed for that deadline. The next `activate_timer()` with the same earliest deadline returns early (`deadline == m_next_deadline`), so nothing re-arms the device and this CPU's timers never fire again. The HPET can refuse this way: `program_min_delta()` returns TimeOut (`kernel/kernel/time/clock_event_device.cpp:223`).

**Cause**
`kernel/kernel/time/timer_queue.cpp:74-75` discards the result with `auto _ =` and caches the deadline unconditionally. In Periodic mode the cache only saves a no-op call, so the discarded result was harmless when it was written.

**Change**
Cache the deadline only when programming succeeded; on failure leave the cache stale-invalid (for example the zero time point) so the next reprogram tries again, and count the failure. Ruled out: asserting success, since a late deadline the device cannot reach is an expected condition, not a bug.

**Reference**
Zircon `TimerQueue::UpdatePlatformTimerMono()` (`zircon/kernel/kernel/timer.cc`) updates `next_timer_deadline_` together with the platform call; Linux `hrtimer_reprogram()` leaves `expires_next` to `tick_program_event()`'s outcome through `clockevents_program_event()`.

**Acceptance**
A kernel test that makes the device refuse once (a fake device, or the HPET with a deadline below its minimum) still sees the timer fire after a later reprogram; both images boot.

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
