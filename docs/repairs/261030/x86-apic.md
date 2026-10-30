# x86-apic outstanding repairs

## 1. x86 stores the TSC's rate twice, in `ApicTimerEvent::TSC_HZ` and in `time::ticks_per_second()`

**Severity**
Low: the two copies are written on adjacent lines from one local, and nothing writes either again, so they cannot differ today. The cost is a second owner of one number, which a later change to either could leave out of step.

**Symptom**
Latent, cannot be triggered today. It would show if the calibration were ever repeated or the rate corrected after boot and one of the two writes were missed: `timershare.test` would divide by one rate while the logger, the lockup reporter and the reset's settle time used the other, and nothing would report the difference.

**Cause**
`apic_calibrate_clock()` (`kernel/arch/x86/apic/local_apic.cpp:671-676`) measures the TSC once and stores the result twice: `ApicTimerEvent::TSC_HZ = tsc_hz;` and `time::set_ticks_per_second(tsc_hz);`. The first is x86's, the second is the generic getter's, which `58383d44` introduced. `arch::Tick` reads the TSC on a PC, so on every PC the two are the same number by construction: the question "at what rate does `arch::Tick` count?" has two owners, which is the shape `docs/repairs/260910/mm-aspace.md` records for the address space. The user kept the second copy for now (2026-10-31, "暂时先同意"), because `TSC_HZ` has a second job that the getter does not have: in TSC-deadline mode it is the APIC timer device's own tick rate, the frequency `init_apic_timer()` enrols the device with (`local_apic.cpp:724`, `USE_TSC_DEADLINE ? TSC_HZ : APIC_HZ`), picked beside `APIC_HZ` as one of a pair.

The copy has one reader besides that enrolment: `apic_tsc_hz()` (`local_apic.cpp:689`, declared in `kernel/arch/x86/kernel/include/ours/arch/apic.hpp:59`), whose only caller is `kernel/commands/timer_share_test.cpp:293`. That file is x86-only and sits in the generic `kernel/commands` (`.agents/tasks/arch-platform-seams/DESIGN.md`, R25 moves it to `kernel/arch/x86/commands`), which is why the getter is an x86 name that a generic file calls.

The two halves of the fix differ in what they wait for. Making `apic_tsc_hz()` forward to the generic getter depends on nothing. Deleting it, so that nothing outside `kernel/arch/x86` names an x86 function for this number, depends on R25: until the test moves, a generic file still calls it.

**Change**
- Make `apic_tsc_hz()` return `time::ticks_per_second()`, and say in its comment that it is the rate of `arch::Tick`, which on a PC is the TSC's. Before the calibration, and after one that failed, both answer 0, as they do today.
- Keep `ApicTimerEvent::TSC_HZ` private to the APIC enrolment: written by the calibration, read only where the device is enrolled, and documented as the device's rate in TSC-deadline mode, not as the counter's rate.
- After R25 moves `timer_share_test.cpp` under `kernel/arch/x86/commands`, have it call `time::ticks_per_second()` and delete `apic_tsc_hz()` with its declaration.

Not chosen here: deleting `TSC_HZ` and having the enrolment read `time::ticks_per_second()` when `USE_TSC_DEADLINE` is set. That leaves one copy, but the user kept the pair `TSC_HZ`/`APIC_HZ` for now; it is the next step if the pair is ever given up.

**Reference**
Zircon keeps no copy of the TSC's rate in its APIC code. `apic_timer_set_tsc_deadline()` (`zircon/kernel/arch/x86/include/arch/x86/apic.h:71`) takes a deadline already in raw ticks, converted by `TimerQueue::ConvertMonotonicTimeToRawTicks()` (`zircon/kernel/kernel/timer.cc:106`) from the one ratio that the platform's TSC calibration sets with `timer_set_ticks_to_time_ratio()` (`timer.cc:77`; called from `zircon/kernel/platform/pc/timer.cc:617`).

**Acceptance**
- `git grep -n 'TSC_HZ\|apic_tsc_hz' -- kernel` shows `TSC_HZ` written once, in `apic_calibrate_clock()`, and read once, in `init_apic_timer()`'s enrolment; `apic_tsc_hz()` is one line over `time::ticks_per_second()`.
- After R25, the same command shows nothing outside `kernel/arch/x86` naming `apic_tsc_hz`, or the function gone.
- x86 `ours-verify` passes, and `scripts/kshell_run.py --out out/bringup.x86 timershare.test` reports `timershare.test: ok`.
