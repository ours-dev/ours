# ktest-fixtures: the fixture batch of the kernel test interface

**Date**: 2026-11-07
**Status**: Proposed

## Request

Feature batch 2 of the kernel test framework (design:
`docs/design/261105/kernel-test-framework.md`, which v1 filed fixtures under
non-goals): `ktest::Fixture` with `SetUp`/`TearDown`, `KTEST_F(Fixture, Name)`
whose body runs with `this` at a fixture instance held in the macro's own
static storage, `KTEST_SUITE_SETUP`/`KTEST_SUITE_TEARDOWN` run once around all
tests of a suite, and a global `ktest::Environment` registered through
`ktest::add_environment`. The runner reports fixture tests exactly like plain
tests; a SetUp failure fails the test and skips the body; TearDown runs even
when assertions failed. No heap: static storage in the macros, the linker
section as the registry.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

None taken outside the request; the judgment calls are recorded in the change
entry (`docs/changes/261107/kernel-test-framework.md`).

## Steps

1. `kernel/lib/ktest/include/ktest/fixture.hpp`: `ktest::Fixture`,
   `ktest::internal::SuiteHook` + `SuitePlan` (the once-per-suite decisions,
   host-testable), `ktest::internal::run_fixture` (the lifecycle the KTEST_F
   adapter expands into), and the `KTEST_F`/`KTEST_SUITE_SETUP`/
   `KTEST_SUITE_TEARDOWN` macros. Records keep the linker-section placement on
   ELF (`__ELF__` guard, no-op on the host, whose tests name the records).
   `ktest.hpp` includes it.
2. `kernel/lib/ktest/include/ktest/environment.hpp` +
   `kernel/lib/ktest/src/environment.cpp`: `ktest::Environment`, a
   fixed-capacity registry, `ktest::add_environment`, and the
   setup-then-reverse-teardown walk the runner and the host tests share.
3. `kernel/lib/ktest/src/runner.cpp`: the `.ktest.suite_hooks` section reader,
   hook framing (a `Context` the hook's assertions record into, no RUN/OK
   line, not counted), the environment passes, and the suite plan threaded
   through the run loop. `kernel/kernel.ld` gains the section beside the hsien
   records.
4. `kernel/lib/ktest/test/fixture_test.cpp`: the host suite. It stands in for
   the kernel-only half of the runner (the six `ktest::internal` functions the
   runner defines) and drives the real macro expansions through `hsien::run`:
   the lifecycle contract, the counting, the suite plan, the environment walk.
   Wired into `kernel/lib/ktest/test/BUILD.gn` (deps gain
   `//system/lib/hsien` and `//kernel/lib/ktest:environment`).
5. `kernel/tests/ktest_fixture_test.cpp`: the booted smoke. `KtestFixture`
   writes a member in SetUp and the bodies check it, a suite-setup counter
   proves once-per-suite, a fresh-fixture pair proves the object is new per
   test, and an environment registered at boot proves it came up before the
   tests. An option-gated `ADD_FAILURE` is the failure-path switch for a
   manual run. Wired into `kernel/tests/BUILD.gn`.

## Files

- `kernel/lib/ktest/include/ktest/fixture.hpp`, `include/ktest/environment.hpp`,
  `src/environment.cpp`, `src/runner.cpp`, `include/ktest/ktest.hpp`,
  `BUILD.gn`, `test/BUILD.gn`, `test/fixture_test.cpp`, `README.md`
- `kernel/kernel.ld`, `kernel/tests/BUILD.gn`, `kernel/tests/ktest_fixture_test.cpp`
- `docs/design/261105/kernel-test-framework.md` (a fixtures section, the way
  run options were added), `docs/changes/261107/kernel-test-framework.md`

## Test plan

- Host: `prebuilt/ninja -C out/bringup.x86 host/obj/kernel/lib/ktest/test/ktest-test`,
  then `python3 scripts/cov_host.py` — the suite green, ≥ 95 % lines on
  `fixture.hpp` and `environment.*`.
- Boot: `python3 .agents/skills/verify/scripts/boot_check.py` (both images, the
  host suite through `//:default`).
- `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest KtestFixture.*'`
  green; the same with `fail=1` for the failure path (TearDown after failure,
  suite teardown still once); `ktest all` still green and `ran` still equals
  the `ktest list` count.
- clang-tidy on every touched C++ file, `format_cpp.py`/`format_gn.py` clean,
  `lint_docs.py` on the documents.

One commit: `feat(ktest): fixtures with SetUp/TearDown, suite setup and a global environment`.
