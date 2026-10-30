# x86-apic outstanding repairs

## 1. x86 stores the TSC's rate twice, in `ApicTimerEvent::TSC_HZ` and in `time::ticks_per_second()`

**Severity**
Low: the two copies are written on adjacent lines from one local, and nothing writes either again, so they cannot differ today. The cost is a second owner of one number, which a later change to either could leave out of step.

**Symptom**
Latent, cannot be triggered today. It would show if the calibration were ever repeated or the rate corrected after boot and one of the two writes were missed: `timershare.test` would divide by one rate while the logger, the lockup reporter and the reset's settle time used the other, and nothing would report the difference.

**Cause**
`apic_calibrate_clock()` (`kernel/arch/x86/apic/local_apic.cpp:671-676`) measures the TSC once and stores the result twice: `ApicTimerEvent::TSC_HZ = tsc_hz;` and `time::set_ticks_per_second(tsc_hz);`. The first is x86's, the second is the generic getter's, which `58383d44` introduced. `arch::Tick` reads the TSC on a PC, so on every PC the two are the same number by construction: the question "at what rate does `arch::Tick` count?" has two owners, which is the shape `docs/repairs/260910/mm-aspace.md` records for the address space. The user kept the second copy for now (2026-10-30, "暂时先同意"), because `TSC_HZ` has a second job that the getter does not have: in TSC-deadline mode it is the APIC timer device's own tick rate, the frequency `init_apic_timer()` enrols the device with (`local_apic.cpp:724`, `USE_TSC_DEADLINE ? TSC_HZ : APIC_HZ`), picked beside `APIC_HZ` as one of a pair.

The copy has one reader besides that enrolment: `apic_tsc_hz()` (`local_apic.cpp:689`, declared in `kernel/arch/x86/kernel/include/ours/arch/x86/apic.hpp:59`), whose only caller is `kernel/commands/timer_share_test.cpp:293`. That file sits in the generic `kernel/commands` (`.agents/tasks/arch-platform-seams/DESIGN.md` lists it under R25 as an x86-only command), so a generic file calls an x86 function for a number the generic getter already holds.

Nothing waits for R25. `time::ticks_per_second()` is declared in the generic `ours/time/ticks_per_second.hpp`, so `timer_share_test.cpp` can call it today, and its `#include <ours/arch/x86/apic.hpp>` (line 59) exists only for `apic_tsc_hz()`. This was checked: a copy of the file with line 293 calling `time::ticks_per_second()` and that include replaced by `<ours/time/ticks_per_second.hpp>` compiles with the build's own flags, and the same copy with only the include replaced fails with `use of undeclared identifier 'apic_tsc_hz'`, so the check can fail. `arch-platform-seams` lists the call (`timer_share_test.cpp:293`) under R3, whose Done-when is that no caller outside `kernel/arch/x86` uses `arch_tick_hz` or `apic_tsc_hz`; R25 changes where the file lives, not whether it may call the generic getter.

**Change**
- In `timer_share_test.cpp:293` call `time::ticks_per_second()`, and drop the `#include <ours/arch/x86/apic.hpp>` (line 59). Before the calibration, and after one that failed, it answers 0 as `apic_tsc_hz()` does, so the `tsc_hz == 0` check below it stays. That include is the file's only x86 header, so this also removes the reason R25 gives for moving it; whether R25 still does is R25's question, not this item's.
- Delete `apic_tsc_hz()` (`local_apic.cpp:689`) and its declaration (`apic.hpp:59`). There is no step that forwards it to the generic getter first: it has one caller, and the caller changes in the same commit.
- Keep `ApicTimerEvent::TSC_HZ` private to the APIC enrolment: written by the calibration, read only where the device is enrolled, and documented as the device's rate in TSC-deadline mode, not as the counter's rate.

Not chosen here: deleting `TSC_HZ` and having the enrolment read `time::ticks_per_second()` when `USE_TSC_DEADLINE` is set. That leaves one copy, but the user kept the pair `TSC_HZ`/`APIC_HZ` for now; it is the next step if the pair is ever given up.

**Reference**
Zircon keeps no copy of the TSC's rate in its APIC code. `apic_timer_set_tsc_deadline()` (`zircon/kernel/arch/x86/include/arch/x86/apic.h:71`) takes a deadline already in raw ticks, converted by `TimerQueue::ConvertMonotonicTimeToRawTicks()` (`zircon/kernel/kernel/timer.cc:107`) from the one ratio that the platform's TSC calibration sets with `timer_set_ticks_to_time_ratio()` (`timer.cc:77`; called from `zircon/kernel/platform/pc/timer.cc:617`).

**Acceptance**
- `git grep -n apic_tsc_hz -- kernel` finds nothing.
- `git grep -n TSC_HZ -- kernel` shows `ApicTimerEvent::TSC_HZ` written once, in `apic_calibrate_clock()` (`local_apic.cpp:672`), and read once, in `init_apic_timer()`'s enrolment (`:724`).
- x86 `ours-verify` passes, and `scripts/kshell_run.py --out out/bringup.x86 timershare.test` reports `timershare.test: ok`.
