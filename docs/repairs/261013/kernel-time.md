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

## 2. Jiffies are kept for one reader, the local APIC's calibration at boot

**Severity**
Low: cleanup; nothing is wrong today.

**Symptom**
With one-shot tick devices, jiffies stand still once the boot CPU leaves the PIT's periodic tick, and nothing reads them after the local APIC's calibration (`apic_calibrate_clock()`, `kernel/arch/x86/apic/local_apic.cpp`). A counter, its writer in the periodic handler (`TickDevice::handle_periodic_event()`'s `do_timer`), `ClockSource::Default::advance_ticks()` and `advance_time()` stay for that one use.

**Cause**
The calibration times its two windows in jiffies (`wait_for_jiffies()`), aligned to a jiffy edge, although the frequencies it records already come from the HPET read at both ends of each window (`Calibration::hz_of()`); jiffies are needed only to say when a window ends, and as the fallback when there is no HPET. Removing the tick Timer (idle-power step 4) left that the last reason for them, and the user chose to keep them for it rather than change the calibration in the same step (decision A, 2026-10-13).

**Change**
Time the windows by the HPET (`SteadyClock::now()`) instead: with the frequency taken from the same reads, no edge alignment is needed. For a machine without an HPET, either poll the PIT's channel 2 for the window, as Zircon does, or refuse to calibrate. Then remove the jiffies counter and its writers. `ClockSource::Default::cpunum()` is "the CPU that advances jiffies" and is what `task::take_cpu_offline()` uses to refuse the boot CPU; it needs a name and a home of its own ("the boot CPU", which takes the global interrupts) before the counter goes.

**Reference**
Zircon `calibrate_apic_timer()` and `pc_init_timer()` (`zircon/kernel/platform/pc/timer.cc`): the APIC timer is calibrated against the HPET when there is one and against the PIT's channel 2, polled, when there is not; Zircon has no jiffies.

**Acceptance**
No jiffies counter in the tree; the boot log's calibration line within 0.1% of today's on dbg; `hotplug.test` still refuses the boot CPU; both images boot.
