# kernel-time outstanding repairs

## 1. Every pick of a thread takes the timer queue's lock and reprograms the timer device

**Severity**
Low: a cost on the context-switch path, not a wrong result.

**Symptom**
Not measured. Each time the scheduler picks a thread, `Scheduler::set_next_thread()` calls `TimerQueue::reset_preempt()`, which takes the queue's spin lock and, because the new deadline (now plus a slice) differs from the one the device holds, writes the device: on the local APIC an MMIO write, under a hypervisor an exit.

**Cause**
The slot is kept under the queue lock so that `reprogram_locked()` can compare it with the timers in one place, and the device is armed for exactly the earliest deadline every time it changes, later or earlier. A later deadline does not need the device at all: the device can fire at the older, earlier one, find nothing due, and arm itself for the right one then.

**Change**
A performance item, so it goes through the user (CLAUDE.md, "Working with the user"). Measure first: the cost of `reset_preempt()` per switch, and the switch rate, with a context-switch benchmark in `benches/`. Candidates:
- Zircon's way: the slot is touched only by its own CPU with interrupts off, so it needs no lock, and the device is written only when the new deadline is earlier than what it holds (`UpdatePlatformTimerMono()`); a later one is picked up when the device fires early;
- keep the lock, skip the write when the deadline is later;
- leave it, if the measurement says the cost is noise.

**Reference**
Zircon `TimerQueue::PreemptReset()` and `TimerQueue::UpdatePlatformTimerMono()` (`zircon/kernel/kernel/timer.cc`): the preemption deadline is written without the timer lock, and the platform timer only moved earlier.

**Acceptance**
The measurement above, and the user's choice among the candidates.
