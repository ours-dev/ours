# kernel-task outstanding repairs

## 1. A reschedule leaves the running thread's local preemption request set, so every hand-off switch is followed by a reschedule that keeps the same thread

**Severity**
Low: a wasted reschedule per switch, not a wrong result.

**Symptom**
`preemptreset.bench` (2026-10-25), two threads handing a token back and forth with semaphores on one CPU: 4,002 switches, and beside them 3,997 reschedules that picked the thread already running, every one in `Mode::Preempt`, counted by a probe in `Scheduler::reschedule_common()` that was not kept. Each sets the CPU's preemption deadline and writes its timer device again, so the deadline is set and the device written 2.00 times per switch instead of once.

**Cause**
A thread that wakes another on its own CPU records a local preemption request on itself (`Scheduler::reschedule_mask()`, `PreemptionState::add_pending()`). The waker then blocks, and its reschedule switches to the thread it woke: the request has been met. `reschedule_common()` sends the remote requests (`flush_pending_remote()`) but keeps the local one. When the waker is switched back in, its next preemption point (`enable_preemption()`, `flush_pending_local()`) finds the request still set and reschedules again, for nothing.

**Change**
Consume the local request in `reschedule_common()`, once the next thread is chosen: the reschedule under way is the one it asked for. A request made after that point, with the run queue lock held and interrupts off, cannot arrive before the switch. `PreemptionState::take_pending_local()` already does it. Measure afterwards with `preemptreset.bench`: 1.00 deadline sets per switch, and no reschedules that keep the same thread in the hand-off.

**Reference**
Zircon `Scheduler::RescheduleCommon()` (`zircon/kernel/kernel/scheduler.cc`): after `EvaluateNextThread()`, `mp_reschedule(preempts_pending())` then `preempts_pending_clear()`, the local bit included.

**Acceptance**
`preemptreset.bench` reports 1.00 deadline sets and device writes per switch; `preempt.test`, `hotplug.test`, `semaphore.test` and `waitqueue.test` pass; both images boot.
