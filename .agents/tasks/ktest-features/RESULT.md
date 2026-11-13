# ktest-features: the excluded features landed, then lost their C shapes and their virtual tables

**Date**: 2026-11-13

## Delivered

Everything the kernel-test-framework design excluded, in five batches plus one
review-driven refactor and one design addendum, all merged to
`baseline/20260824`:

- Runner options — `ktest all shuffle[=seed]`, `repeat=N`, `shard=M/N`
  (251dcf77, merged 19f22e17).
- Fixtures — `KTEST_F` with `ktest::Fixture` (`SetUp`/`TearDown`), suite setup
  and teardown, global environments (8c8b0adc, merged 19f22e17).
- Parameterized and typed tests — `KTEST_P` over `KTEST_VALUE_PARAM`,
  `KTEST_T` over `KTEST_TYPE_LIST` (7a682a9e, merged 19f22e17).
- Death tests — `KTEST_DEATH` records in `.ktest.death`, one fresh boot per
  test, host-orchestrated per the design's sketch (c1898922, merged 19f22e17).
- Matchers — `KTEST_EXPECT_THAT` with the core set (f46707a2, merged 19f22e17).
- Review-driven refactor of the features' C-shaped APIs — `Environment` stage
  virtuals, `RunPlan::from_words` over a `Result`, the shared stage runner in
  `src/run_stage.hpp`; preceded by the bounded-format panic fix it surfaced
  (bb214da4, e088031a, merged 43f0425f; record
  `docs/changes/261113/ktest.md`).
- Design addendum, the user's 2026-11-13 request: the fixtures become CRTP —
  static stage dispatch, one static instance per record, no virtual table and
  no per-run placement construction (merged with the CRTP commit; record
  `docs/changes/261113/ktest.md`).

## Verification

- Every batch: host suite ≥95% lines by `cov_host.py`, booted run green,
  `boot_check.py` dbg+rel+host green — per-batch numbers in the change records
  (`docs/changes/261111/`, `261112/`, `261113/`).
- Final state: `boot_check.py --out out/bringup.x86` build ok, dbg PASS
  46/5/0, rel PASS 44/8/0 (7/7 required), host-tests 183/0; host `ktest-test`
  83/83; `ktest all` on the dbg image `ran=83 passed=80 failed=0 skipped=3`
  (three known skips, see below).

## Deviations from the design

- Deviation in the plan's step 2: the fixture base is CRTP, not polymorphic, and the
  instance is one static object per record rather than a fresh construction
  per run — the user's 2026-11-13 addendum. Consequence documented in the
  macro's comment: fixture state persists across a suite's tests and across
  `repeat` passes; a test needing fresh state resets in the body or in SetUp.
- `KTEST_P`/`KTEST_F_P` keep per-record storage plus `run_fixture`: each
  record's object must be constructed with its table value, which a shared
  static instance cannot do.

## Found along the way

- The bounded-format panic: `ktest all shuffle repeat=2 shard=0/1` panicked at
  the tip in logz4's allocating format buffer — any run printing more than the
  small fixed buffer, and the panic path itself shared the hazard. Fixed
  before the refactor landed (bb214da4), recorded in
  `docs/changes/261113/kernel-log.md`.
- The features' first API shapes were C idioms (function-pointer-plus-void*
  environment steps, out-parameter parsers, three hand-written digit loops) —
  found by review and refactored into the object/`Result` shapes the codebase
  otherwise uses (43f0425f).
- Preempt-bounds flakiness on the emulator — pre-existing, filed
  `docs/repairs/261108/preempt-bounds.md` during the migration round; the
  suites skip it under the emulator.

## Not done

- Death tests orchestrate one boot per test from the host; booting from the
  kernel's own kshell remains out of scope, per the design.
- Matchers scope to the core set the suites use (comparison, null, describe);
  a wider grammar is deferred until a suite needs it.
