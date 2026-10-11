# kernel-time outstanding repairs

## 2. Two owners program the same tick device: SchedClock::reset_preempt() bypasses TimerQueue

**Severity**
Medium: latent, its only caller is Fair under `sched_nohz`, which is off (`kernel/kernel_params.gni`).

**Symptom**
Latent, cannot be triggered today. With the device in one-shot mode, `SchedClock::reset_preempt()` (`kernel/kernel/task/sched/sched_clock.cpp:44-55`) programs `TickDevice::Current` directly. It can push the device past a nearer timer's deadline, and `TimerQueue::m_armed_deadline` then names a deadline the device is no longer armed for, so that timer is late until something else reprograms. Since 2026-10-10 a debug build stops on that call with an assertion when the device is one-shot (`docs/changes/261010/kernel-time.md`), so it fails loudly instead of mis-arming; a release build still does the latter.

**Cause**
The per-CPU event device has two writers that do not know about each other: the timer queue, which caches what it armed, and the scheduler's preemption deadline. Whoever writes last wins.

**Change**
Make the timer queue the only writer: keep the preemption deadline as a slot inside `TimerQueue` and arm the device for the earlier of that slot and the queue's head, keeping `SchedClock::reset_preempt()`'s interface so `fair.*` is untouched. Depends on the one-shot work in `.agents/tasks/high-res-timers/DESIGN.md`.

**Reference**
Zircon `TimerQueue::PreemptReset()` and `UpdatePlatformTimerLocked()` (`zircon/kernel/kernel/timer.cc`): the slice end is a field of the timer queue, and the platform timer is armed for the earliest of it and the timer lists.

**Acceptance**
With one-shot mode and `sched_nohz` on, a timer due before the next preemption still fires on time (a kernel test logging both deadlines); both images boot.
