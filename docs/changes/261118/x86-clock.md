# x86-clock changes

## APIC calibration trusts the clock source when the tick loses periods, instead of refusing the APIC timer (2026-11-18)

**Files**
- `kernel/arch/x86/apic/local_apic.cpp` — `apic_calibrate_clock()`: the tick-period cross-check warns and keeps the clock-source frequencies instead of refusing at 2x; a clock source that runs backwards and an APIC timer below 1 MHz are refused
- `docs/repairs/261117/x86-clock.md` — deleted, its only item is fixed

**Symptom**
On GitHub's 3-core macos-26 runner, which boots dbg and rel side by side while the host tests run (run 44230112165), both boots logged `tick period: 39609947 ns measured by the clock source, 10000000 ns expected` followed by the error `APIC calibration: the tick device and the clock source disagree by more than a factor of two; the tick is misprogrammed`. No CPU then got an APIC timer (`APIC timer not calibrated on CPU[1], skip enrolling it`), `boot_check.py` failed both images on the error line, and rel's profiler test reported `profile checks FAILED` (1 kernel sample). On an idle Mac the tick measures 10.0-10.2 ms and nothing happens.

**Cause**
Calibration uses the periodic tick (PIT, or HPET in legacy mode) as its ruler: it waits for 1 tick and then for 20 ticks, and reads the APIC current count, the TSC and the clock source (HPET) at both ends of each window. Two different things come out of those reads:

- The APIC and TSC frequencies, from `Calibration::hz_of()`: count divided by the clock source's elapsed time over the same window. This is a ratio of counters read at the same instants, so it does not depend on how long the window was or on the tick being right.
- The tick period, from the clock source's time over the 19-tick difference. This is the only number that does depend on the tick.

The old code treated a tick period more than 2x off 1/HZ as proof that the tick device was misprogrammed, and threw away the frequencies too. But under emulation there is a third way for the tick to be long that the 2x rule did not consider: lost ticks. QEMU's emulated PIT and HPET schedule their periodic interrupts on host time. When the QEMU vCPU thread is descheduled -- three cores, two QEMUs, a parallel host-test run -- the periods that passed meanwhile are coalesced into one interrupt instead of being delivered late. The kernel counts one jiffy, but 3-4 periods of HPET time have gone by. So the windows were simply longer than planned, and the frequencies measured over them were still right; refusing them turned a harmless stretch into "no CPU has a timer". The 2x threshold had been set against the 7% noise between idle TCG boots, which is noise, a different failure from lost periods.

The general lesson: when a measurement has two independent rulers, ask which result depends on which ruler before letting a disagreement veto everything. Here only the tick period depended on the tick; the answer actually used depended only on the clock source.

**Change**
The cross-check stays, but a disagreement is reported, not acted on. Before:

```cpp
if (drift * 10 > expected_tick) {
    log::warn("tick period is {}% off what the clock source says",
              (drift * 100) / expected_tick);
}
if (drift > expected_tick) {
    log::error("APIC calibration: the tick device and the clock source disagree by "
               "more than a factor of two; the tick is misprogrammed");
    return false;
}
```

After (abridged):

```cpp
if (drift * 10 > expected_tick) {
    auto const periods = u64((mono_span + expected_tick / 2) / expected_tick);
    if (measured_tick > expected_tick) {
        log::warn("APIC calibration not consistent with the clock source: {} ticks "
                  "spanned {} tick periods, so about {} ticks were lost; using the "
                  "clock source", SPAN, periods, periods - u64(SPAN));
    } else {
        log::warn("... spanned only {} tick periods, so the tick runs fast; ...");
    }
}
```

The warning says how many ticks were lost (tick periods by the clock source minus ticks counted), at warn level, so `boot_check.py` no longer fails a boot over it. Calibration still refuses the cases that really are broken:

- the APIC counter ran out inside the window, or the counters did not advance (unchanged);
- the clock source ran backwards between the two windows (`mono_span < 0`, new: previously that fell into the 2x refusal by accident, and with the refusal gone it would have produced a garbage frequency from a negative divisor cast to `u64`);
- an APIC timer below 1 MHz or a zero TSC frequency (the APIC floor is new and replaces "APIC frequency is zero"): every real or emulated local APIC counts a bus or core clock of tens of MHz and up, so a frequency under 1 MHz means the measurement itself is wrong, most likely the clock source's scale.

Options the user rejected (repair item 1, candidates 2 and 3): timing both windows with the clock source directly and dropping the periodic tick from calibration (declined earlier for jiffies, 2026-10-23, see `kernel-design` task.md), and making the CI host quieter (hides the bug). The user chose this one.

**Reference**
Linux `calibrate_APIC_clock()` with `calibrate_by_pmtimer()` in `arch/x86/kernel/apic/apic.c`: it counts `LAPIC_CAL_LOOPS` (HZ/10) ticks of the global clock event device, measures the same window with the ACPI PM timer, and when the PM timer's reading is more than 1% off 100 ms prints "APIC calibration not consistent with PM-Timer: %ldms instead of 100ms" and rescales the APIC delta and the TSC delta to the PM timer ("APIC delta adjusted to PM-Timer"). It then skips the jiffies-based verification of the APIC timer when the PM timer was used (`pm_referenced`). Its sanity floor `lapic_timer_period < (1000000 / HZ)` ("APIC frequency too slow, disabling apic timer") is the 1 MHz floor here. Two differences: this kernel's frequencies already come from the clock source by ratio, so there is nothing to rescale, only the refusal to drop; and the warning threshold is 10%, not 1%, because successive idle boots under TCG already differ by 7%.

**Verification**
- Build of `out/bringup.x86` clean; `scripts/format_cpp.py --check` clean; clang-tidy reports nothing on the changed lines.
- `boot_check.py` on an idle host: dbg PASS (46 passed, 5 skipped), rel PASS (44 passed, 8 skipped, 7/7 required), host tests 185 passed; tick measured 10.13 ms (dbg) and 10.07 ms (rel), no warning, APIC 1 GHz.
- Reproduced the CI failure locally on the 15-core Mac before the fix: `SMP=4`, 90 busy-loop Python processes running while `boot_check.py --no-build --no-tests` booted both images. Ticks measured 36.9 ms (dbg) and 28.0 ms (rel), the 2x error fired in both, the APs logged `APIC timer not calibrated`, both images FAIL. With 30 hogs at the default CPU count the tick only stretched to 13.9 and 18.0 ms (38% and 79%), under the old 2x line.
- After the fix, same load: dbg tick 16.0 ms, "19 ticks spanned 30 tick periods, so about 11 ticks were lost", rel tick 41.1 ms, "19 ticks spanned 78 tick periods, so about 59 ticks were lost" (more than the 4x seen on GitHub); both still calibrated APIC 1000000000 Hz, TSC 999998xxx Hz, and both PASS with 0 error lines, rel's 7/7 required checks (the profiler among them) included.

| Run | Tick measured | Lost | APIC Hz | Verdict |
|---|---|---|---|---|
| idle, before | 10.0-10.2 ms | 0 | 1000000000 | PASS |
| loaded, before (dbg / rel) | 36.9 / 28.0 ms | not reported | refused | FAIL / FAIL |
| loaded, after (dbg / rel) | 16.0 / 41.1 ms | 11 / 59 | 1000000000 | PASS / PASS |
| idle, after (dbg / rel) | 10.13 / 10.07 ms | 0 | 1000000000 / 999994991 | PASS / PASS |

Not yet verified on the GitHub runner itself; the parent session reruns CI after merging.

**Deliberately not done**
- No check that the short and long windows give the same frequency. It would catch a counter that changes rate mid-window, but Linux has no such check and the existing "ran out" and "did not advance" checks cover the stopped-counter case; it was not worth a new threshold to tune.
- The tick device's lost periods are not corrected anywhere: jiffies still undercount under load. Nothing reads jiffies once a CPU is one-shot, and the periodic build is not used by the dev images; the warning makes it visible.
- No skip of calibration via CPUID leaf 0x15/0x16 or a hypervisor-reported frequency, which Linux also does: a separate feature, not this repair.

**How it was found**
CI run 44230112165 failed both boots; its downloaded serial logs showed the tick at 34.8 ms (dbg) and 39.6 ms (rel) followed by the 2x error. Locally the same images measured 10.0 ms, which pointed at host load rather than a kernel bug in the tick device. `hz_of()`'s comment and the log's `APIC timer` line from loaded-but-under-2x boots (1 GHz exactly, with the tick 79% long) showed the frequencies were unaffected by the stretch.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a lost tick make the calibration window longer but leave `hz_of()`'s APIC frequency correct?
- Under what host conditions does an emulated periodic device coalesce interrupts, and why would a real PIT on idle hardware not do this?
- What failure was the old 2x refusal meant to catch, and which of the remaining checks still catches a clock source whose scale is wrong?
