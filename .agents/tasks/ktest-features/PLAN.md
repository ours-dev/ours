# ktest-features: the design's excluded features, now wanted

**Date**: 2026-11-07
**Status**: Proposed

## Request

The user (2026-11-07): implement everything the kernel-test-framework design excluded — fixtures (SetUp/TearDown, suite setup, global environment), parameterized tests, typed tests, matchers, shuffle/repeat/sharding, and death tests (the design's host-orchestrated shape, `.ktest.death`).

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Order of work | fixtures first / death first / runner options first | runner options (smallest) → fixtures → parameterized+typed → death → matchers (largest) | None | as recommendation, 2026-11-07 |

## Steps

Each step lands as one commit with its host tests and booted smoke test; every step keeps `ktest all` green (61 tests) and the per-feature host suite at ≥95% lines.

1. Runner options: `ktest all shuffle[=seed]`, `repeat=N`, `shard=M/N` — runner-level, no test-side API. Verified: `test_kshell_run.py` cases + a booted `ktest all shuffle repeat=2` run.
2. Fixtures: `KTEST_F(Suite, Name)` with a `ktest::Fixture` base (`SetUp()`/`TearDown()` virtual), `KTEST_SUITE_SETUP`/`KTEST_SUITE_TEARDOWN` per suite, and a global environment pair. No heap: fixture objects live in the test's own storage (BSS/static), one instance per test, constructed before SetUp.
3. Parameterized + typed: `KTEST_P(Suite, Name)` over a value table (`KTEST_VALUE_PARAM`), and `KTEST_T(Suite, Name)` over a type list; the runner reports each instantiation as its own `TEST` line.
4. Death tests: `KTEST_DEATH(Suite, Name)` records in `.ktest.death`, per the design's host-orchestrated sketch — the host launches a fresh boot per death test, the assertion expects the panic, zero kernel recovery.
5. Matchers: `EXPECT_THAT(value, matcher)` with the core set (eq/ne/lt/le/gt/ge, is_null, throws-equivalent none — freestanding). Largest step; scope its sub-grammar to what the tests actually use.
6. Records: change records per batch, RESULT at the end, merge to baseline.

## Files

- `kernel/lib/ktest/**` — the features, their host suites
- `kernel/tests/**` — smoke tests exercising each feature on target
- `scripts/kshell_run.py` / `scripts/test_kshell_run.py` — verdict grammar extensions for the runner options
- `docs/design/261107/ktest-features.md` — the design note the steps cite

## Test plan

Per step: host suite ≥95% lines (cov_host), booted run via kshell_run, boot_check dbg+rel, `ktest all` stays green.
