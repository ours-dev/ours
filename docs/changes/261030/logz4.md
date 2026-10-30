# logz4 changes

## Title: log_console_waits() reads the counter's rate once and converts counts to microseconds without a wrapping multiply (2026-10-30)

**Files**
- `kernel/lib/logz4/kernel.cpp` — `log_console_waits()` loads `time::ticks_per_second()` once and takes the tick period from that one value instead of calling `tick_period()` (a second load); its `us()` converts counts as whole seconds times a million plus the remainder times a million, and the comment above it is rewritten to be arithmetically right

**Symptom**
Two latent faults, neither reachable in a normal run.
- After enough accumulated console waiting, the `lockup status` / `log_console_waits()` lines print a wrapped, too small number of microseconds. `WriteAccount` adds every wait's counts into `wait_ticks` for the whole boot, and the old `us()` multiplied that total by a million before dividing. A 64-bit word holds 1.8e19, so the product wraps once the total passes 1.8e13 counts: about 5 hours of summed lock waiting at 1 GHz, about 1 hour at 5 GHz.
- If the rate went from 0 to non-zero between the function's two loads, `us()` would divide by 0. Only the kernel shell calls the function, long after the calibration, so the window is closed in practice.

**Cause**
The comment above `us()` said "a counter at 5 GHz for a day is about 4.3e14 counts, and a million times that still fits in 64 bits". The first half is right (5e9 x 86400 = 4.3e14), and the second half is not: 4.3e14 x 1e6 is 4.3e20, and the largest u64 is 1.8e19, so it is 24 times too big. The comment was a checked-looking sentence nobody had checked, and it argued the code safe with a number that did not hold. A claim in a comment about overflow is worth exactly the multiplication behind it, so redo the multiplication: the headroom of `ticks * 1'000'000` is `2^64 / 1e6 = 1.8e13` counts, which at 1 GHz is 18,000 seconds of counter, not a day.

The tempting fix is to widen: do the product in `unsigned __int128`. That is more than the problem needs, and a 128-bit divide is a call to a compiler runtime helper (`__udivti3`) on a path that two 64-bit divisions serve. A count is `seconds x rate + remainder` with `remainder < rate`, so
`ticks * 1e6 / rate  =  (ticks / rate) * 1e6  +  (ticks % rate) * 1e6 / rate`.
The first term multiplies whole seconds, so it wraps only after 1.8e13 seconds of counting. The second multiplies something below the rate, so it fits whenever `rate * 1e6` does, that is for any rate up to about 1.8e13 Hz, three orders of magnitude above anything a CPU counts at. The two terms are exact together (no rounding is lost: the remainder is divided once, last).

The second fault is the ordinary double-read mistake on a value that is not constant. `ticks_per_second()` is documented as monotone, 0 and then one number for good. Two calls are two observations, and the pair can be (0, N) as well as (N, N). Code that tests one observation and divides by the other has no guard at all. The shape that has none of this is to read once into a `const` local and derive everything else from that local. `log_console_waits()` read the rate once as `tick_hz` for `us()` and again, through `tick_period()`, for the guard `period == 0`; with the first read 0 and the second N, the guard passes and `us()` divides by 0.

**Change**
Before:

```cpp
auto const tick_hz = time::ticks_per_second();
auto const period = tick_period();
if (period == 0) {
    log::info("console: the tick counter's rate is not known yet, so no times can be given");
    return;
}
// Microseconds: a counter at 5 GHz for a day is about 4.3e14 counts, and
// a million times that still fits in 64 bits.
auto const us = [tick_hz](u64 const ticks) -> u64 { return ticks * 1'000'000 / tick_hz; };
```

After:

```cpp
auto const tick_hz = time::ticks_per_second();
auto const period = tick_hz / static_cast<u64>(time::HZ);
if (period == 0) {
    log::info("console: the tick counter's rate is not known yet, so no times can be given");
    return;
}
auto const us = [tick_hz](u64 const ticks) -> u64 {
    return ticks / tick_hz * 1'000'000 + ticks % tick_hz * 1'000'000 / tick_hz;
};
```

`period == 0` still guards the divisions: `period != 0` implies `tick_hz >= HZ > 0`, so `tick_hz` is never 0 in `us()`, and `ticks_past_tick / period` is safe. The tick period is the same expression `tick_period()` has; `tick_period()` stays for the per-write path in `SerialLogger::write()`, which has no second value to disagree with.

Rejected:
- `unsigned __int128` for the product, for the reason above.
- Changing `tick_period()` to take the rate as a parameter, so that both callers share the formula. It would touch the logger's per-line path for the sake of a diagnostic command; the formula is one division.

**Reference**
Zircon's `affine::Ratio::Scale()` (`zircon/system/ulib/affine/ratio.cc:200`, which calls `ScaleUInt64()` at `ratio.cc:55`) meets the same overflow without a wider type: it splits the value into two 32-bit halves, multiplies each by the 32-bit numerator, divides the high half first and carries its remainder into the low half. It is the same quotient-and-remainder idea as here, with two differences: its ratio is a reduced numerator and denominator of 32 bits each, where this code has one integer rate, and it saturates at the limit instead of wrapping.

**Verification**
- Arithmetic, checked in Python against exact integers: the old form gives 553,255,926 us for 1.9e13 counts at 1 GHz (it should be 19,000,000,000: wrapped), the new one gives 19,000,000,000; 100,000 random counts below 2^50 at each of 10 MHz, 1 GHz and 5 GHz agree with the exact `ticks * 1e6 // rate`.
- x86 `ours-verify`: build ok (out/bringup.x86, 1.6s); dbg PASS shell finished at 6.1s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 5.4s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- `kshell_run.py --out out/bringup.x86 "lockup status"`: ok; the four `console: cpuN wrote ... waited 2.636 ms in all, the longest 0.068 ms ...` lines print the same shape as before, so the common case is unchanged (e.g. cpu0 2.636 ms in all).
- `format_cpp.py --check` clean; `clang-tidy -p out/bringup.x86` reports nothing in `kernel/lib/logz4/kernel.cpp`.
- Not measured on a run that really accumulates 1.8e13 counts of waiting: nothing here waits for five hours.

**Deliberately not done**
- `tick_period()` is not changed and `SerialLogger::write()` keeps calling it.
- No other place in this file turns counts into a time: `read_time()` hands the raw count to the logger, and the panic lock bound (`panic_lock_ticks()`) divides the rate by 1000 before it multiplies, so neither has the product that wrapped.

**How it was found**
An independent review of the three commits that introduced `ticks_per_second()` (`58383d44`, `146a0ece`, `b023807f`) re-did the comment's multiplication and found it did not fit. The double load was found by reading the lines next to the one `58383d44` changed.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `(ticks / rate) * 1'000'000 + (ticks % rate) * 1'000'000 / rate` exact, and why does its second product fit in 64 bits where `ticks * 1'000'000` did not?
- The rate is documented as 0 and then one number for good. Why is that not enough to make two loads of it safe to mix, and what does the code do instead?
- Which counter at which rate would first have shown the wrapped number, and after how long?
