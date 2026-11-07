# ktest-params: parameterized and typed tests for ktest

**Date**: 2026-11-07

## Delivered

`KTEST_VALUE_PARAM(Suite, int, MyValues, 1, 42, 99)` + `KTEST_P`/`KTEST_F_P`
(one record per value, named `Suite.Name/N`, the body reading `GetParam()`),
`KTEST_TYPE_LIST(Suite, (A, B))` + `KTEST_T` (one record per type, the body
templated on `T`), all records constexpr parts of the image in the existing
`.hsien.tests` section and none of it needing a runner change. Committed as
one change on `exp/ktest-migration`: `feat(ktest): parameterized and typed
tests`.

## Verification

- Host: `ktest-test` 60 cases green (14 new in `instantiate_test.cpp`); the
  runner stand-in moved to `test/host_shim.{hpp,cpp}`, every fixture check
  preserved. `scripts/cov_host.py`: `instantiate.hpp` 100 % lines (15/15),
  `fixture.hpp` 96.5 % (137/142).
- Boot: `boot_check.py` green — dbg 46/5/0, rel 44/8/0, host-tests 183/0.
- `ktest list` names 77 (was 67): the 10 new instantiations by name.
  `ktest all`: `ok, 77 ran, 75 passed, 0 failed, 2 skipped` — `ran` equals the
  list count, no verdict-shape change.
- `ktest KtestParam.*` 6/6, `ktest KtestParamFixture.*` 2/2, `ktest
  KtestTyped.*` 2/2; one instantiation `ktest KtestParam.ValueIsPositive/1`
  1/1; `ktest all shard=76/77` exactly one record; `ktest all shuffle=1
  shard=1/77` shuffled over instantiation indices, one green record.
- clang-tidy clean on every touched file (only the pre-existing
  `hsien/test.hpp` noise); `format_cpp.py`/`format_gn.py` clean;
  `lint_docs.py` on the changed documents.

## Deviations from the design

- The design doc filed parameterized and typed tests under non-goals; this
  batch adds them to it the way the fixture batch did, with a paragraph in the
  Design section.
- `GetParam()` was chosen over a `value_` member (the task left the pick
  open): it is the GoogleTest spelling the API model follows, and it cannot
  collide with a fixture member in the composed macro.
- The typed body is a free static function template over `T`, not a
  fixture-class member; `KTEST_F_T` is deliberately not done this round.

## Found along the way

- An in-class `static constexpr` member's initializer cannot instantiate its
  own class's member templates — the records block is now built by calling the
  block builder from the namespace-scope variable, after the class completes.
- A macro parameter named `name` is substituted inside the field designator
  `.name` — `KTEST_T`'s parameters are spelled `suite_token`/`name_token`.
- The consteval block builders removed the last 6 uncovered lines of
  `instantiate.hpp` from the coverage count: constexpr functions are emitted
  and instrumented as runtime code even when only ever run at compile time.

## Not done

- `KTEST_F_T` (fixture + typed), a middle name segment for instantiations,
  per-instantiation value printers, and diagnostics beyond the
  `RecordBlock` `static_assert` for empty tables — recorded under "Deliberately
  not done" in the change entry (`docs/changes/261107/kernel-test-framework.md`).
