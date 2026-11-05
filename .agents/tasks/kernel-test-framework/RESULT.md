# kernel-test-framework: the kernel test interface on the hsien runtime, with the pilot migrated

**Date**: 2026-11-05

## Delivered

- `b208036d` — `docs(kernel-test-framework)`: the design accepted into `docs/design/261105/kernel-test-framework.md`, and `PLAN.md`.
- `484e5273` — `feat(ktest)`: `kernel/lib/ktest` with the `KTEST` declaration, the `EXPECT`/`ASSERT`/`KTEST_SKIP` families, the selection grammar, the message formatting, the runner and the `ktest` console command; `.hsien.tests` moved from `.init.rodata` to `.data.rel.ro`; `kernel.tests` linked to the library; host gtest for the filter, the formatting and the summary.
- `3ed64780` — `feat(ktest)`: `kernel_test()` the GN template, the interface's three smoke tests, and the image swap — `vmours.with-tests` in `kernel.dev.dbg_1` (kernel shell, `KERNEL_DEBUG_LEVEL=1`), the rel image back on plain `vmours`; every comment and document that named the pair flipped in the same change.
- `df23558b` — `feat(ktest)`: `scripts/kshell_run.py` parses the runner's grammar (summary, the list-count check for a run of everything, the list verdict, the NMI/lockup diagnosis for a hung run) with 20 unit tests in `scripts/test_kshell_run.py`.
- `9534e49b` — `feat(ktest)`: the three-test pilot (`SamplerClaim.HoldsClaimUntilMemoryIsGone`, five `Semaphore.*`, `VmaWx.MapRefusesWriteAndExecute`), the stale comments in the two tests that stay commands, `scripts/cov_kernel.py` learning the registry from a booted `ktest list`, the `vmours.with-tests` kernel ELF path, and `docs/repairs/261105/kernel-test-framework.md` for the remaining 43 commands.
- `b7c9a288` — `fix(ktest)`: the self-review's fixes (atomic publication, copied skip reason, owner check before dereference, host verdict on `ktest: ERROR`, summary arithmetic, the C-string note, the README path) and `.agents/tasks/kernel-test-framework/REVIEW.md`.
- This document and `PLAN.md`'s final entry: `docs(ktest): result of the kernel test interface`.

The tree now has: 10 kernel tests reachable by `ktest`, the host verdict that fails a partial run, coverage that counts the migrated tests, and a repair list for the rest.

## Verification

- `python3 .agents/skills/verify/scripts/boot_check.py` (final): build ok in 4.4 s; dbg PASS, shell finished at 5.8 s, 46 passed / 5 skipped / 0 failed, no error line; rel PASS, shell finished at 5.1 s, 44 passed / 8 skipped / 0 failed, 7/7 required; host-tests PASS, 173 passed / 0 failed.
- `ktest list` on the dbg image: `Ktest.AssertionsPass`, `Ktest.SkipIsReported`, `Ktest.Names_MayHaveUnderscores`, `SamplerClaim.HoldsClaimUntilMemoryIsGone`, the five `Semaphore.*`, `VmaWx.MapRefusesWriteAndExecute` — 10 tests.
- `ktest all`: `SUMMARY ran=10 passed=9 failed=0 skipped=1`, verdict `ok, 10 ran, 9 passed, 1 skipped`, exit 0, with the SKIP line carrying the copied reason.
- The pilot suites on their own, all exit 0: `ktest SamplerClaim.*` (45.7 ms), `ktest VmaWx.*` (859 us), `ktest Semaphore.*` (434 us, 1.7 ms, 5.6 ms, 2.0 ms, 26.5 ms).
- The old command names are gone; `samplerclaim.test` fails as an unknown command (expected after the migration).
- The final dbg kernel's `.hsien.tests` bounds 10 records (240 bytes) inside `.data.rel.ro`; the rel kernel carries none.
- `python3 scripts/test_kshell_run.py`: 20 tests; `python3 scripts/test_cov_kernel.py`: 19 tests; both all passing.
- `python3 scripts/cov_kernel.py --out out/cov-kernel`: exit 0 after the 10 interface tests and the 43 remaining commands. The dbg kernel's report is 74.4% lines (42,153 of 56,631), `kernel/lib/ktest` 42.3% (142/336), `system/lib/hsien` 52.5% (21/40), and the areas the migrated tests reach: `kernel/lib/sampler` 80.4%, `kernel/object` 90.3%, `kernel/mm` 82.0%, `kernel/kernel/sync` 86.1%. Not like-for-like with the design's earlier 68.1% (26,894 of 39,491 on 2026-10-09); the tree and the denominator grew in between.
- Failed paths, measured: `ktest NoSuch.*` ends at the error line in 7.4 s (was 120 s) and the host fails with the error line named; the skip-reason fix was checked with a stack-buffer reason against a booted run.
- `clang-tidy` on every changed C++ file: no diagnostic; `format_cpp.py --check`, `format_gn.py --check`, `lint_docs.py --changed` and `gn check` clean.
- Not measured: the wall time of `ktest all` against running the three commands by hand (the console is the same either way and the set was never run together before); the wrong-thread assertion path (it needs a deliberately misbehaving test, and the fix is the absence of a race rather than an output change); the hang path end to end.

## Deviations from the design

- Step 1 wired `kernel.tests` to `//kernel/lib/ktest` a step earlier than the plan said, because the kernel-toolchain target has no other edge that would compile it; recorded in `PLAN.md` step 1 and in the change record.
- The host ends a `ktest` run on an `ktest: ERROR` line as well as on the summary. The design named the summary as the terminal line; the registry-level errors (empty registry, duplicate name, usage error) print no summary, and the review item M2 showed the host would wait 120 s and NMI the machine for a registry error.
- The semaphore command migrated as five `Semaphore.*` tests rather than one test per command body: its five aspects were already five separate check functions, and one test per aspect is what the interface is for. All checks are preserved.
- `EXPECT_EQ` does not special-case C strings the way GoogleTest's `FormatForComparison` does; the difference is documented in `assertions.hpp` and the README, and implementing it is a later printer/matcher increment.
- Test sources are excluded from the default coverage report (the existing `cov_host` behaviour; `--all` counts them), so `kernel/tests/` has no row in the coverage table.
- Everything the design called a non-goal stayed out: fixtures, parameterized and typed tests, matchers, death tests, per-test isolation and timeouts, running tests from `verify`, and the annotation scanner. `UNITTESTS_ENABLED` is still unread, as decision 10 said.

## Found along the way

- The `KTEST` macro's first expansion was broken twice over: a parameter named `name` substituted into the `.name` designator, and the anonymous namespace's closing brace sat between the body declaration and the author's `{`. Both fixed and recorded in the step-2 change record.
- `clang-tidy`'s `UPPER_CASE` rule rejected the internal macros' trailing underscore; they are `KTEST_EXPECT_TRUE` and friends now.
- `scripts/format_gn.py` called `kernel/products/bin/BUILD.gn` unsafe because its two imports were not in gn's order; sorted while editing the file.
- `ktest::internal::finish()` was not `inline`, which only a link of three translation units including the header could show; fixed in step 4.
- `EXPECT_EQ(count, 0)` compared `u64` with `int` and failed `-Werror`; the literals are typed now.
- The self-review's findings and their fates are in `REVIEW.md` and the fifth change record; the two left open are the `unit_tests_enabled` metadata (a decision, not an edit) and `make_buffer`'s zero-room precondition (documented, internal).

## Not done

- The remaining 43 `*.test` console commands, in `docs/repairs/261105/kernel-test-framework.md`, including the three that take arguments (`interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `timershare.test big`) and need a decision on per-run configuration first.
- The design's non-goals and open questions: fixtures, parameterized/typed tests, matchers, death tests, per-test isolation and timeout, `verify` integration, `UNITTESTS_ENABLED`, and reviving the pending annotation scanner.
- The optional hand-written-function exercise for the user (the filter glob or the host verdict) was offered with this task and not taken; `docs/authored.md` is unchanged.
