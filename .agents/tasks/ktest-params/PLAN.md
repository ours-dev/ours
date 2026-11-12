# ktest-params: parameterized and typed tests for ktest

**Date**: 2026-11-07
**Status**: Proposed

## Request

Feature batch 3 of the kernel test framework (step 3 of
`.agents/tasks/ktest-features/PLAN.md`, on the branch that carries it; design:
`docs/design/261105/kernel-test-framework.md`, which v1 filed parameterized and
typed tests under non-goals). `KTEST_VALUE_PARAM(Suite, int, MyValues, 1, 42, 99)`
declares a named constexpr value table; `KTEST_P(Suite, Name)` registers one
record per value, named `Suite.Name/0`, `Suite.Name/1`, ... (GoogleTest's
suffix convention); `KTEST_TYPE_LIST(Suite, (A, B))` plus `KTEST_T(Suite, Name)`
does the same over a type list, the body templated on the type. The runner
reports each instantiation exactly like a plain test; filters match the full
name including the `/N` suffix; `KTEST_F_P` composes with fixtures if cheap.
No heap: constexpr tables, static storage, records in the existing section.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|

None taken outside the request; the judgment calls are recorded in the change
entry (`docs/changes/261107/kernel-test-framework.md`).

## Steps

1. `kernel/lib/ktest/include/ktest/fixture.hpp`: `run_fixture` takes trailing
   construction arguments (`Args &&...args` forwarded to `std::construct_at`),
   so a parameterized test's object can be built with its value. `KTEST_F`'s
   call passes none; its behavior is unchanged.
2. `kernel/lib/ktest/include/ktest/instantiate.hpp`: the shared machinery —
   `internal::InstantiationName` (a compile-time `Suite.Name/N`),
   `internal::RecordBlock<N>` (the constexpr records array that keeps the
   section's flat walk intact), `internal::TypeList` — and the macros:
   `KTEST_VALUE_PARAM` (the named table plus the per-suite param base the test
   class derives from), `KTEST_P`/`KTEST_F_P` (one inner macro; the fixture
   base is `::ktest::Fixture` or the author's fixture), `KTEST_TYPE_LIST`,
   `KTEST_T`. `ktest.hpp` includes it.
3. Host suite `kernel/lib/ktest/test/instantiate_test.cpp`: expansion shape and
   naming, value-table iteration per record, type-list expansion, filter
   matching over `/N` suffixes, summary counting over a whole block. The
   runner stand-in (the six `ktest::internal` functions and the shared state)
   moves to `test/host_shim.hpp`/`host_shim.cpp` so two test files can drive
   records; `fixture_test.cpp` keeps every check and includes the shim.
4. Booted smoke `kernel/tests/ktest_param_test.cpp`: a `KTEST_P` over 3 values
   checking each value, a `KTEST_F_P` proving fixture + parameter compose on
   target, and a `KTEST_T` over 2 types checking through `T`.
5. Documents: the design doc's fixtures section gains the parameterized
   paragraph, the README a user-facing section, and the change record lands in
   `docs/changes/261107/kernel-test-framework.md`.

## Files

- `kernel/lib/ktest/include/ktest/fixture.hpp` — `run_fixture` construction
  arguments.
- `kernel/lib/ktest/include/ktest/instantiate.hpp` — new: the machinery and the
  five macros.
- `kernel/lib/ktest/include/ktest/ktest.hpp`, `kernel/lib/ktest/BUILD.gn`,
  `kernel/lib/ktest/test/BUILD.gn` — wiring.
- `kernel/lib/ktest/test/host_shim.hpp`, `host_shim.cpp` — new: the runner
  stand-in shared by the host files that run records.
- `kernel/lib/ktest/test/fixture_test.cpp` — loses its private shim, keeps
  every check.
- `kernel/lib/ktest/test/instantiate_test.cpp` — new: the host suite.
- `kernel/tests/ktest_param_test.cpp`, `kernel/tests/BUILD.gn` — the booted
  smoke.
- `kernel/lib/ktest/README.md`, `docs/design/261105/kernel-test-framework.md`,
  `docs/changes/261107/kernel-test-framework.md` — the documents.

## Test plan

- Host: `prebuilt/ninja/ninja -C out/bringup.x86
  host/obj/kernel/lib/ktest/test/ktest-test`, then `python3
  scripts/cov_host.py` — the suite green, ≥ 95 % lines on `instantiate.hpp`.
- Boot: `python3 .agents/skills/verify/scripts/boot_check.py` (both images, the
  host suite through `//:default`).
- `ktest list` names every instantiation; `ktest KtestParam.*` and
  `ktest KtestTyped.*` green through `scripts/kshell_run.py`; a filter with a
  `/N` suffix runs exactly one instantiation; `ktest all` stays green with
  `ran` equal to the list count.
- clang-tidy on every touched C++ file; `format_cpp.py`/`format_gn.py`/
  `lint_docs.py` clean.

One commit: `feat(ktest): parameterized and typed tests`.
