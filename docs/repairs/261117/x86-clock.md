# x86-clock outstanding repairs

## 1. APIC calibration refuses the APIC timer when the emulator drops periodic ticks, leaving every CPU without a timer

**Severity**
Medium: on an unloaded host it never triggers (the tick measures 10.0 ms against 10 ms expected), but on a loaded one -- the 3-core GitHub runner booting two QEMUs at once -- the kernel runs on with no APIC timer, and the profile test fails.

**Symptom**
GitHub run 44230112165 (macos-26, 2026-11-18), both images:

```text
tick period: 39609947 ns measured by the clock source, 10000000 ns expected
tick period is 296% off what the clock source says
APIC calibration: the tick device and the clock source disagree by more than a factor of two; the tick is misprogrammed
APIC timer not calibrated on CPU[1], skip enrolling it
```

dbg measured 34.8 ms, rel 39.6 ms. The error line fails both boots in `boot_check.py`, and rel's shell then reports `profile checks FAILED` (1 kernel sample, 6 user samples).

**Cause**
`calibrate()` in `kernel/arch/x86/apic/local_apic.cpp` uses the periodic tick as its ruler: it waits 1 tick and then 20 ticks, reads the HPET clock source and the APIC counter around each window, and cross-checks the tick period (HPET time over 19 ticks) against 1/HZ. Under TCG the emulated PIT/HPET deliver their periodic interrupts on host time; when QEMU's thread is descheduled, missed periods are coalesced into one interrupt instead of being delivered late, so each counted tick spans 35-40 ms of HPET time instead of 10 ms: about three periods in four were never delivered. The cross-check reads that as a misprogrammed tick device and refuses the whole calibration, although `apic_hz` and `tsc_hz` come from `hz_of()`, a ratio of two counters read at the same instants, and are still right: the windows were longer than intended, not wrong. The 2x threshold was chosen against 7% boot-to-boot noise seen under TCG on an idle host; a loaded host loses ticks, which is a different failure from noise.

**Change**
To be decided by the user. Candidates:
- Trust the clock source when the two disagree: keep the APIC and TSC frequencies from the ratio, report the tick device's lost ticks as a warning, and refuse only when the ratio itself is inconsistent (Linux's choice).
- Calibrate without the periodic tick: time both windows with the clock source directly (spin on HPET reads for 10 ms and 200 ms), so lost tick interrupts cannot stretch the ruler.
- Leave the kernel and make the CI host quieter (boot the two images one after the other, or `-icount` so virtual time follows executed instructions): hides the problem rather than fixing it.

**Reference**
Linux `calibrate_APIC_clock()` with `calibrate_by_pmtimer()` (`arch/x86/kernel/apic/apic.c`): it counts `LAPIC_CAL_LOOPS` ticks of the global clock event device, then checks the result against the ACPI PM timer, and when they disagree by more than 1% it prints "APIC calibration not consistent with PM-Timer" and rescales the APIC and TSC values to the PM timer instead of refusing. Linux also skips this calibration entirely when CPUID leaf 0x15/0x16 or the hypervisor reports the frequency (`lapic_init_clockevent()`, `lapic_timer_period` preset).

**Acceptance**
`python3 scripts/ci.py all` passes on the GitHub macos-26 runner with dbg and rel booted side by side; locally, a boot under artificial host load (for example `SMP=4` with a CPU hog on every host core) logs `APIC timer: ... Hz` and no `APIC calibration` error.
