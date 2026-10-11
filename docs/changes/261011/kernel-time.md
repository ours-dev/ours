# kernel-time changes

## timershare.test divides each repeat by its own unloaded timing (2026-10-11)

**Files**
- `kernel/commands/timer_share_test.cpp` — the share of a repeat is its unloaded timing over its loaded one; the median and the best of the five are reported

**Symptom**
`timershare.test` failed once in the fifteen or so runs of one session: "the thread kept under 25.0 % of its CPU in 1 run(s)". That run's C = 20 µs point reported a median share of 19.6 % and, on the same line, "the work alone takes 9086 µs", where every other run said about 23 000 µs. The kernel had not changed.

**Cause**
The statistic compared two different populations. The reference was the fastest of five unloaded timings and the numerator's partner was the median of five loaded ones. Under QEMU TCG the speed of the guest drifts with the host's load and frequency, so within one point the spin loop ran at a different speed from one repeat to the next. In the failing run one unloaded repeat caught a fast spell and ran in 9 ms; the loaded repeats ran at the usual speed, and 9 ms over the usual loaded time looked like 19.6 %. A minimum is the right reference when noise can only slow a run down; it is wrong here, because the drift also speeds runs up, and the comparison needs the two timings to have had the same conditions.

The share is now taken per repeat: an unloaded timing followed at once by a loaded one, divided, five times, and the five shares are sorted. The drift between a pair is small next to the drift across the whole point, and the median of the ratios no longer depends on which repeat was lucky.

**Change**
Before, in `measure()`:

```cpp
auto const kept_median = 1000 * run->base_tsc / run->loaded_tsc[REPEATS / 2];
```

After:

```cpp
shares[rep] = 1000 * run->base_tsc[rep] / run->loaded_tsc[rep];
// ... sorted ...
auto const kept_median = shares[REPEATS / 2];
```

Ruled out: raising the 25 % floor's tolerance until the flake stopped. That would have hidden a statistic that was wrong, and a floor that is wrong in one direction is wrong in the other.

**Reference**
None, this project's own measurement.

**Verification**
- Six runs on the one-shot build after the change, QEMU 11.1.1 TCG, 4 CPUs, P = 10 µs: all pass, and the medians of the three points are 60.5 / 47.1 / 50.2, 50.1 / 51.7 / 47.6, 54.4 / 54.7 / 54.4, 61.7 / 58.5 / 46.3, 61.3 / 53.2 / 50.3 and 49.9 / 53.5 / 48.1 %. The range is 46 to 62 %, as the four runs before it gave (45 to 54 %).
- `scripts/format_cpp.py --check` and `clang-tidy -p out/bringup.x86` on the file: clean, no diagnostic in it.
- Not measured: the failing run cannot be replayed, since the host's speed at that moment is not recorded. That the cause is the statistic is shown by the 9086 µs on the failing line and by six runs of the corrected one, not by reproducing the failure.

**Deliberately not done**
- Recording the unloaded timing of every repeat in the log line. The line already reports the median of them as "the work alone takes", which is what made this failure legible.

**How it was found**
Running the test together with four others in one session and reading the failing run's line in `dbg.log`: the "work alone" figure was 2.5 times lower than in every other run.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the minimum the right reference for a timing when noise can only slow it down, and why was it wrong here?
2. Why is a ratio taken within one repeat less sensitive to the host's drift than a ratio of statistics taken across five?
3. What would a floor loosened until the flake went away have hidden?
