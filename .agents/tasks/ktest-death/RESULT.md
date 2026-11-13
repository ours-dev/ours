# ktest-death: death tests, one fresh boot per test

**Date**: 2026-11-08

## Delivered

`KTEST_DEATH(Suite, Name)` — a `hsien::TestRecord` in the new `.ktest.death`
section the live registry never walks; `ktest list` prints the death registry
under `ktest: DEATH` lines after the live tests; `ktest death [filter]` runs
the matching death tests on the kernel side, printing the DEATH line before
each invoke and reporting a returned body FAILED; `ktest death` on the host
reads the list off one boot and gives each test a fresh boot of its own,
judging PASS by the panic report that follows the marker and FAIL by the
runner's own SUMMARY (the body returned) or a hang. Committed as one change
on `exp/ktest-migration`: `feat(ktest): death tests, one fresh boot per test`.

## Verification

- Host: `test_kshell_run.py` 36 cases green (15 new: classification, ends-the-
  machine, the filter grammar, the list read, the end pattern, and six
  `judge_ktest_death` verdicts); the old 21 untouched.
- ELF: `vmours.with-tests` holds `__ktest_death_start`..`__ktest_death_end` =
  0x18 bytes — exactly one record; the plain `vmours` (the rel image's kernel)
  holds neither symbol, so the section is absent there, the `.hsien.tests`
  rule.
- Boot: `boot_check.py` green (dbg 46/5/0, rel 44/8/0, host-tests 183/0).
- `ktest list`: `ok, 77 test(s)`, the last line `ktest: DEATH
  KtestDeath.PanicsOnDemand`. `ktest all`: `ok, 77 ran, 75 passed, 2 skipped`
  — unchanged width, the death test counted nowhere.
- `ktest death`: the setup boot reads the list, then one fresh boot —
  `ktest: DEATH KtestDeath.PanicsOnDemand`, the other CPUs suspended,
  `OURS KERNEL PANIC`, `[... cpu0 error] ktest: death test reached its panic`
  — verdict `ok`; overall `ktest death: ok, 1 death test(s) panicked as
  expected, one boot each`. `ktest death KtestDeath.*`: same PASS. `ktest
  death Nope.*`: `FAIL, no death test matches "Nope.*" among the 1 listed`,
  no reboot.
- Failure path by hand: a temporary `KTEST_DEATH(KtestDeath, SurvivesOnDemand)`
  with an empty body — the runner printed the DEATH line, then `ktest: FAILED
  KtestDeath.SurvivesOnDemand (a death test must panic; it returned)` and
  `SUMMARY ran=1 passed=0 failed=1 skipped=0`; the host judged `FAIL, the
  boot is alive -- the death test did not panic` and the overall verdict was
  `FAIL, 1 of 2 death test(s) did not die as expected`. Removed before the
  commit.
- clang-tidy clean on every touched file (the pre-existing unrelated-header
  noise is unchanged); `format_cpp.py`/`format_gn.py` clean;
  `lint_docs.py` on the changed documents.

## Deviations from the design

None in shape — the host-orchestrated design sketch is implemented as
written. Two details the sketch left open, decided here and recorded in the
change entry: the death selection takes one optional filter and no options,
and a body that returns is reported FAILED with a SUMMARY (so the host has a
line to end on), rather than leaving the machine at the shell silently.

## Found along the way

None.

## Not done

- `KTEST_EXPECT_PANIC("message")` (the design sketch's fifth point): a form
  that pins the expected panic message per test. The host cannot know the
  message a record panics with — the record type is `hsien::TestRecord` and
  the design keeps it that way — so a message check needs either a side
  table or a record extension; neither is needed while one smoke test
  exists. Recorded in the change entry.
- Death tests on any image but the dbg one: the rel kernel links no test
  registry at all, and the host loop reads the dbg image's list.
