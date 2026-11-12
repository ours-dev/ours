# ktest-fixtures: the fixture batch of the kernel test interface

**Date**: 2026-11-07

## Delivered

`KTEST_F` with `ktest::Fixture` (SetUp/TearDown, the object in the macro's own
static storage), `KTEST_SUITE_SETUP`/`KTEST_SUITE_TEARDOWN` in a new
`.ktest.suite_hooks` section, `ktest::Environment` + `ktest::add_environment`
with a fixed-capacity registry, and the runner threading both around the run.
Committed as one change on `exp/ktest-migration` (hash added by the commit
itself): `feat(ktest): fixtures with SetUp/TearDown, suite setup and a global
environment`.

## Verification

- Host: `ktest-test` 46 cases green (24 new); `scripts/cov_host.py` 183/183.
  Coverage of the new code: `fixture.hpp` 96.5 % lines (136/141),
  `environment.cpp` 100 % (35/35), `environment.hpp` pure declarations.
- Boot: `boot_check.py` green — dbg 46/5/0, rel 44/8/0, host-tests 183/0.
- `ktest KtestFixture.*`: `ok, 3 ran, 3 passed, 0 skipped`. `ktest all`:
  `ok, 67 ran, 65 passed, 2 skipped` (`ran` equals the `ktest list` count, 67).
- Failure path: `ktest KtestFixture.* fail=1` fails the first test, its
  TearDown still runs (the third test's teardown counter proves it), the suite
  teardown still comes once after the suite, and the run ends
  `SUMMARY ran=3 passed=2 failed=1` with the host verdict FAIL.
- `gn check` (whole build) OK; clang-tidy clean on the touched files;
  `format_cpp.py`, `format_gn.py`, `lint_docs.py` clean.

## Deviations from the design

- The design doc filed fixtures under non-goals; this batch adds them to it
  the way run options were added, with the section in
  `docs/design/261105/kernel-test-framework.md`.
- Suite-hook records live in a new `.ktest.suite_hooks` section rather than
  `.hsien.tests` (the runner must not mistake hooks for tests) — the
  second-section pattern the design already sketched for death tests.
- `SuitePlan` and `EnvironmentRun` are host-testable pieces (kernel/lib/ktest
  headers plus one new `environment.cpp` TU), so the once-per-suite and
  once-per-run contracts are host-covered, not only booted.

## Found along the way

- Two host environment cases registered stack objects without restoring the
  registry, so a later walk called into destroyed objects. The coverage build
  crashed on it; the normal build survived silently. Fixed with a
  `RegistryReset` guard in every registering case.
- `kshell_run.py` and the run-plan batch are another session's concurrent
  work on this branch; this change builds on top of it and its hunks stay out
  of this commit.

## Not done

- `RecordProperty`, parameterized/typed tests, matchers, death tests — the
  design's non-goals, unchanged.
- The ASSERT/EXPECT distinction inside SetUp: any recorded failure skips the
  body (the runner cannot tell them apart), recorded in the change entry.
