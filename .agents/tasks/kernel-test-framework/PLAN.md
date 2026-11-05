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

## Files

- `kernel/lib/ktest/{BUILD.gn,README.md,.clang-format,.gitignore}`, `kernel/lib/ktest/include/ktest/{ktest,assertions,run_state,format,filter,summary}.hpp`, `kernel/lib/ktest/src/runner.cpp`, `kernel/lib/ktest/test/{BUILD.gn,filter_test.cpp,format_test.cpp,summary_test.cpp}`: new (step 1).
- `kernel/kernel.ld`: the `.hsien.tests` block moves between output sections (step 1).
- `kernel/tests/BUILD.gn`: `kernel.tests` depends on `//kernel/lib/ktest` (step 1).
- `BUILD.gn` (root): `//kernel/lib/ktest:tests($host_toolchain)` joins `//:default` (step 1).
- `docs/changes/261105/kernel-test-framework.md`: the change record for step 1.

## Test plan

- Host gtest: the filter grammar (`*`, `?`, `:`, `-`, an empty positive half, no match, match all), the failure-message formatting (a printable value, a value with no `std::formatter`, user format arguments, truncation and the nul terminator), and the counting the SUMMARY line reports. Done in step 1: 9 cases, all passing.
- Kernel toolchain: the source set builds with the kernel's `-fno-exceptions -fno-rtti` and its freestanding libc++ headers. Done in step 1 under `kernel_x86.dbg.0`; the dbg.1 build edge arrives with the image swap in step 2.
- Boot: both images still boot and the boot lines are unchanged, because nothing in the boot path reaches the library. Done in step 1.
- The section's placement in `.data.rel.ro` was checked on the unstripped test kernel in step 1, where the (empty) range sits between `__hsien_tests_start` and `__hsien_tests_end`; the non-empty case is checked in step 2, with the first record.
