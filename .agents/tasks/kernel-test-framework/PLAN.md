# kernel-test-framework: plan for the kernel test interface on the hsien runtime

**Date**: 2026-11-05
**Status**: Proposed

## Request

Build the in-kernel test interface the design describes — `KTEST(Suite, Name)`, gtest/ktest-style assertions and filters, a `ktest` console command with a parseable `SUMMARY`, run on demand on the dbg image — as a kernel-facing layer on hsien's existing runtime. The user approved the design and its decisions on 2026-11-05: reuse hsien's runtime rather than write a second one, `EXPECT` continues and `ASSERT` returns from the current function, tests run on demand from the dbg image's kernel shell, and v1 is the interface plus a three-test pilot with the other 43 `*.test` commands filed in `docs/repairs/`. Scope ends at the pilot: fixtures, parameterized tests, matchers, death tests, per-test isolation and running from `verify` stay out, as the design's non-goals say. The design lives at `docs/design/261105/kernel-test-framework.md`.

## Decisions

All are decided on 2026-11-05; the options, reasons and the user's choices are in the Decisions table of `docs/design/261105/kernel-test-framework.md`. None is open.

## Steps

Later steps are written when each one is introduced to the user, not before.

1. The interface library and its host tests. Landed in `484e5273`. `kernel/lib/ktest`, wired into the test-kernel link edge, with no test registered yet.
   - `kernel/lib/ktest/include/ktest/`: `ktest.hpp` (`KTEST(Suite, Name)` emits the body function, the one-line adapter that matches hsien's `void (*)(hsien::Context &)` invoke, and one `hsien::TestRecord` in `.hsien.tests`), `assertions.hpp` (`EXPECT_*`/`ASSERT_*`/`ADD_FAILURE`/`FAIL`/`KTEST_SKIP` format a message into a fixed stack buffer and call the running `hsien::Context::check`, with `ASSERT_*` adding a `return;`), `run_state.hpp` (the per-test state the adapter binds and the macros read), and `filter.hpp`/`format.hpp`/`summary.hpp`, which carry no kernel dependency so the host tests compile them.
   - `kernel/lib/ktest/src/runner.cpp`: the runner, the failure reporter and the `ktest` `GKTL_COMMAND`. It walks `hsien::section_begin()`/`hsien::section_end()`, selects with the filter grammar, calls `hsien::run` one record at a time, and prints the `RUN`/`OK`/`FAILED`/`SKIP`/`SUMMARY` lines.
   - `kernel/kernel.ld`: the existing `.hsien.tests` block moves from `.init.rodata` to `.data.rel.ro`, keeping its `KEEP` and its `__hsien_tests_start`/`__hsien_tests_end` symbols. This is the one change on the runtime side, and it is what lets the runner read the records after init has been reclaimed.
   - `kernel/lib/ktest/BUILD.gn`: a `cpp_interface("headers")`, the kernel `source_set("ktest")` (`testonly = true`) depending on `//system/lib/hsien`, `//kernel/lib/gktl:headers`, `//kernel/lib/logz4:headers` and the header surfaces the current-thread read reaches through, plus `test:ktest-test` and a `group("tests")` wired into the root `//:default` as `//kernel/lib/ktest:tests($host_toolchain)`.
   - `kernel/tests/BUILD.gn`: `kernel.tests` now depends on `//kernel/lib/ktest`. That is what gives the kernel-toolchain source set a build edge and a place in the test-kernel link; the aggregate still holds no tests, so the command answers "no tests are linked into this kernel" until the next step.
   - Why now: every later step builds on the macro/record/assertion contract, and this step was verifiable without booting — the host tests cover matching, formatting and counting, and the library compiles in the kernel toolchain. It touches no boot path, so a mistake here could not break `verify`.
   - Verification, as done: `python3 .agents/skills/verify/scripts/boot_check.py` — build ok in 52.1 s; dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (173 passed, 0 failed; the new executable is one, 9 cases). The source set builds in `kernel_x86.dbg.0`; the dbg.1 with-tests toolchain has no edge until the image swap next step. The ELF was already checked because `kernel.tests` links the library: `__hsien_tests_start == __hsien_tests_end` inside `.data.rel.ro` in the test kernel, and the non-test kernel carries none of it. `cpp-write-and-verify` (clang-format and clang-tidy) clean on the changed files, `scripts/format_gn.py` clean, `scripts/lint_docs.py --changed` clean, `gn check` OK on the host and kernel targets.
   - Landed as one commit, `484e5273`, `feat(ktest): the kernel test interface library on the hsien runtime`.

2. The first records on the dbg image. Landed in `3ed64780`. The test-capable kernel moved to the image that can run it, and the interface's own smoke tests became the first records.
   - `kernel/tests/kernel_test.gni`: `kernel_test("name")` makes a testonly source set whose include surface is `//kernel/lib/ktest:headers`; `kernel.tests` aggregates the targets and still links the runtime.
   - `kernel/tests/ktest_test.cpp`: `Ktest.AssertionsPass` (every assertion family, all passing), `Ktest.SkipIsReported` (`KTEST_SKIP`) and `Ktest.Names_MayHaveUnderscores` (a test name may carry `_`, unlike a console command name).
   - `kernel/products/image/dev.dbg_1/BUILD.gn` packages `vmours.with-tests` built in dbg.1, and its kernel package and image are `testonly`; `kernel/products/image/dev/BUILD.gn` packages plain `vmours(dbg.0)` and its kernel package lost its testonly. The comments and docs that named the pair were flipped in the same change: `kernel/products/bin/BUILD.gn`, `kernel/kernel_image.gni`, `products/{bringup.gni,README.md}`, `kernel/lib/lockup_detector/README.md`, `tools/gen_debug_menu.py`, `.agents/skills/verify/SKILL.md`, `docs/development/debugging.md`.
   - Two fixes came out of building the first test: a `KTEST` macro parameter named `name` substituted into the `.name` designator (the parameters are `suite_token`/`name_token`/`line_number` now, and the anonymous namespace closes before the body), and `clang-tidy`'s `UPPER_CASE` rule rejected the internal macros' trailing `_` (they are `KTEST_EXPECT_TRUE` and friends). `scripts/format_gn.py` called `kernel/products/bin/BUILD.gn` unsafe over its unsorted imports; they were sorted.
   - Why now: the registry and the image edge are what every later step measures against — the host verdict, the pilot migration and the coverage run all need a booted `ktest` run.
   - Verification, as done: `boot_check.py` (dbg PASS 46/5/0 at 6.1 s, rel PASS 44/8/0 with 7/7 required, host-tests 173/0); `ktest list` prints the three names; `ktest all` prints two `OK`, one `SKIP`, `SUMMARY ran=3 passed=2 failed=0 skipped=1` and exits 0; `ktest Ktest.Assertions*` prints `ran=1 passed=1`; `ktest NoSuch.*` prints the `ERROR` line and `SUMMARY ran=0`, and the host exits 1; the dbg kernel's `__hsien_tests_start`/`__hsien_tests_end` bound three 24-byte records inside `.data.rel.ro` while the rel kernel has none; tidy, format, GN layout and docs lint clean.
   - Landed as one commit, `3ed64780`, `feat(ktest): run the first kernel tests on the dbg image`.

3. The host verdict for `ktest`. Landed in `df23558b`. `scripts/kshell_run.py` parses the runner's grammar instead of judging it as an ordinary command.
   - A run ends at `ktest: SUMMARY`; exactly one summary must be present, `failed` must be 0 and `ran` non-zero. For `ktest` and `ktest all` the script first types `ktest list` and requires `ran` to equal the listed count; a filter run has no expected count, because the glob stays in the kernel. `ktest list` passes on the prompt plus at least one `TEST` line; a missing summary fails; the legacy `*.test`, `crash` and `lockup` rules are untouched.
   - While a `ktest` command runs, console silence is a hang, so a stuck run is NMI'd and the lockup report is the diagnosis; the script drains `AFTER_VERDICT` after the command like after a `*.test`.
   - The verdict helpers are pure over the captured lines (`is_ktest`, `ktest_lists`, `ktest_runs_all`, `ktest_summaries`, `ktest_listed`, `judge_ktest_list`, `judge_ktest_run`), which `scripts/test_kshell_run.py` exercises with 17 cases without QEMU.
   - Why now: the pilot migration replaces commands with `ktest` tests; the host has to fail a partial run before that, or the migration loses the verification the commands had.
   - Verification, as done: unit tests 17/17; `ktest all` (`3 ran, 2 passed, 1 skipped`, exit 0), `ktest Ktest.Assertions*` (`ran=1`, exit 0), `ktest NoSuch.*` (`FAIL, SUMMARY says no test ran`, exit 1), `ktest list` (ok), `samplerclaim.test` unchanged; `boot_check.py` passes both images and 173 host tests.
   - Landed as one commit, `df23558b`, `feat(ktest): the host verdict parses the kernel test runner's SUMMARY`.

4. The three-test pilot, and coverage follows the registry. Landed in `9534e49b`. The first tests that are not the interface's own moved from the console-command form.
   - `kernel/tests/{sampler_claim,semaphore,vma_wx}_test.cpp` -- moved from `kernel/commands/` and converted. `samplerclaim.test` becomes one `KTEST(SamplerClaim, HoldsClaimUntilMemoryIsGone)`, whose "another session holds the claim" path is `KTEST_SKIP`; `semaphore.test` becomes five `Semaphore.*` tests, one per aspect, with the worker threads still only publishing counters for the test thread; `vmawx.test` becomes `KTEST(VmaWx, MapRefusesWriteAndExecute)`. Every check survived, and fatal setup paths became named `ASSERT_*`.
   - `kernel/commands/BUILD.gn` drops the three files; `kernel/tests/BUILD.gn` gains their `kernel_test()` targets and the header dependencies they reach.
   - `kernel/commands/{user_copy_test,mutex_stress}.cpp`: the comments that said the tree has no in-kernel test runtime now say the test has not migrated.
   - `scripts/cov_kernel.py` asks the booted kernel for its tests (`ktest list`) and runs them one at a time beside the remaining `*.test` commands; its kernel ELF is `vmours.with-tests` now; `scripts/test_cov_kernel.py` covers the list parser.
   - The conversion surfaced an ODR bug: `ktest::internal::finish()` in `format.hpp` was not `inline`, which only a link of three translation units including it could show. It is inline now.
   - Why now: this is the design's v1 end -- the interface is measured against real subsystem tests, and the coverage consumer no longer loses a test when it migrates.
   - Verification, as done: `ktest list` lists ten tests; `ktest all` reports `SUMMARY ran=10 passed=9 failed=0 skipped=1` and exits 0; `ktest SamplerClaim.*`, `ktest VmaWx.*` and `ktest Semaphore.*` all pass; the old command names fail as unknown commands (the expected cost of the rename); `boot_check.py` passes both images and 173 host tests; `scripts/test_cov_kernel.py` 19/19; coverage run numbers are in the change record; tidy, format, GN layout and docs lint clean, `gn check` OK on the migrated targets.
   - Landed as one commit, `feat(ktest): migrate the three pilot tests and teach coverage to find tests`.

## Files

- `kernel/lib/ktest/{BUILD.gn,README.md,.clang-format,.gitignore}`, `kernel/lib/ktest/include/ktest/{ktest,assertions,run_state,format,filter,summary}.hpp`, `kernel/lib/ktest/src/runner.cpp`, `kernel/lib/ktest/test/{BUILD.gn,filter_test.cpp,format_test.cpp,summary_test.cpp}`: new (step 1).
- `kernel/kernel.ld`: the `.hsien.tests` block moves between output sections (step 1).
- `kernel/tests/BUILD.gn`: `kernel.tests` depends on `//kernel/lib/ktest` (step 1).
- `BUILD.gn` (root): `//kernel/lib/ktest:tests($host_toolchain)` joins `//:default` (step 1).
- `docs/changes/261105/kernel-test-framework.md`: the change record for step 1.

- `kernel/tests/kernel_test.gni`, `kernel/tests/ktest_test.cpp`: new (step 2).
- `kernel/tests/BUILD.gn`: the `framework` target and the aggregate's dependencies (step 2).
- `kernel/products/image/dev.dbg_1/BUILD.gn`, `kernel/products/image/dev/BUILD.gn`: the image swap (step 2).
- The pairing comments and docs (`kernel/products/bin/BUILD.gn`, `kernel/kernel_image.gni`, `products/{bringup.gni,README.md}`, `kernel/lib/lockup_detector/README.md`, `tools/gen_debug_menu.py`, `.agents/skills/verify/SKILL.md`, `docs/development/debugging.md`) (step 2).
- `kernel/lib/ktest/include/ktest/{assertions,ktest}.hpp`: the macro fixes and renames (step 2).

- `scripts/kshell_run.py`, `scripts/test_kshell_run.py`: the `ktest` verdict and its unit tests (step 3).
- `.agents/skills/verify/SKILL.md`: the kernel-tests section names the `ktest` command (step 3).

- `kernel/tests/{sampler_claim,semaphore,vma_wx}_test.cpp`: moved and converted from `kernel/commands/` (step 4).
- `kernel/commands/BUILD.gn`, `kernel/tests/BUILD.gn`: the three commands leave, the three test targets arrive (step 4).
- `kernel/commands/{user_copy_test,mutex_stress}.cpp`: the stale "no runtime" comments (step 4).
- `kernel/lib/ktest/include/ktest/format.hpp`: `finish()` is `inline` (step 4).
- `scripts/cov_kernel.py`, `scripts/test_cov_kernel.py`: the booted `ktest list` discovery and the `vmours.with-tests` ELF (step 4).
- `docs/repairs/261105/kernel-test-framework.md`: the remaining 43 commands (step 4).

## Test plan

- Host gtest: the filter grammar (`*`, `?`, `:`, `-`, an empty positive half, no match, match all), the failure-message formatting (a printable value, a value with no `std::formatter`, user format arguments, truncation and the nul terminator), and the counting the SUMMARY line reports. Done in step 1: 9 cases, all passing.
- Kernel toolchain: the source set builds with the kernel's `-fno-exceptions -fno-rtti` and its freestanding libc++ headers. Done in step 1 under `kernel_x86.dbg.0`; the dbg.1 build edge arrives with the image swap in step 2.
- Boot: both images still boot and the boot lines are unchanged, because nothing in the boot path reaches the library. Done in step 1.
- The section's placement in `.data.rel.ro` was checked on the unstripped test kernel in step 1, where the (empty) range sits between `__hsien_tests_start` and `__hsien_tests_end`; the non-empty case is checked in step 2, with the first record.
- Booted run: `ktest list`, `ktest all`, a filter and a no-match error on the dbg image, with the expected `RUN`/`OK`/`SKIP`/`ERROR`/`SUMMARY` lines and exit status. Done in step 2.
- ELF of the images: the dbg kernel bounds three 24-byte records in `.data.rel.ro`; the rel kernel carries none. Done in step 2.
- Host verdict (step 3): the 17 unit tests, plus the four booted cases (`ktest all`, a filter, a no-match and `ktest list`) and an unchanged `*.test` command. Done in step 3.
- Pilot migration (step 4): `ktest list` lists ten tests; `ktest all` reports `ran=10 passed=9 skipped=1`; each `ktest <Suite>.*` passes; the old command names fail. Done in step 4.
- Coverage (step 4): `python3 scripts/cov_kernel.py` runs the ten interface tests and the remaining commands, and reports the migrated files' lines. Numbers in the change record.
