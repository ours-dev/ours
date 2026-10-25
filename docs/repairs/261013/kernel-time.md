# kernel-time outstanding repairs

## 1. Every pick of a thread takes the timer queue's lock and reprograms the timer device

**Severity**
Low: a cost on the context-switch path, not a wrong result.

**Symptom**
Counted with `preemptreset.bench` on 2026-10-25 (`benches/kernel/time/`): two threads handing a token back and forth on one CPU set the deadline and write the device once per pick, once per switch (2.00 per switch before the stale preemption request was consumed, `docs/changes/261025/kernel-task.md`). A pick reads the steady clock twice, once in `SchedClock::reset_preempt()` and once in `ClockEventDevice::program_event()`, and the clock is the HPET here. Timed under QEMU's TCG only, which models none of the costs (the bench's `all`): a clock read 450 ns, a spin lock 190 ns, `reset_preempt()` with the deadline unchanged 330 ns and with a new one 4,280 ns, a switch 9.1 us (17.6 us with the stale request). On hardware an HPET read is usually quoted at 0.5 to 1 us, a local APIC write at 0.1 to 0.2 us; not measured here. Each time the scheduler picks a thread, `Scheduler::set_next_thread()` calls `TimerQueue::reset_preempt()`, which takes the queue's spin lock and, because the new deadline (now plus a slice) differs from the one the device holds, writes the device: on the local APIC an MMIO write, under a hypervisor an exit.

**Cause**
The slot is kept under the queue lock so that `reprogram_locked()` can compare it with the timers in one place, and the device is armed for exactly the earliest deadline every time it changes, later or earlier. A later deadline does not need the device at all: the device can fire at the older, earlier one, find nothing due, and arm itself for the right one then.

**Change**
A performance item, so it goes through the user (CLAUDE.md, "Working with the user"). The counts are measured (above); the costs need hardware. Candidates:
- reuse the clock reading the scheduler took on entry, in `reset_preempt()` and in the device's programming, which saves one or two clock reads a pick and changes nothing else;
- Zircon's way: the slot is touched only by its own CPU with interrupts off, so it needs no lock, and the device is written only when the new deadline is earlier than what it holds (`UpdatePlatformTimerMono()`); a later one is picked up when the device fires early;
- keep the lock, skip the write when the deadline is later;
- leave it, if the measurement says the cost is noise.

**Reference**
Zircon `TimerQueue::PreemptReset()` and `TimerQueue::UpdatePlatformTimerMono()` (`zircon/kernel/kernel/timer.cc`): the preemption deadline is written without the timer lock, and the platform timer only moved earlier.

**Acceptance**
The measurement above, and the user's choice among the candidates.
