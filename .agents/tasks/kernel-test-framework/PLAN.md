# kernel-test-framework: plan for the kernel test interface on the hsien runtime

**Date**: 2026-11-05
**Status**: Proposed

## Request

Build the in-kernel test interface the design describes — `KTEST(Suite, Name)`, gtest/ktest-style assertions and filters, a `ktest` console command with a parseable `SUMMARY`, run on demand on the dbg image — as a kernel-facing layer on hsien's existing runtime. The user approved the design and its decisions on 2026-11-05: reuse hsien's runtime rather than write a second one, `EXPECT` continues and `ASSERT` returns from the current function, tests run on demand from the dbg image's kernel shell, and v1 is the interface plus a three-test pilot with the other 43 `*.test` commands filed in `docs/repairs/`. Scope ends at the pilot: fixtures, parameterized tests, matchers, death tests, per-test isolation and running from `verify` stay out, as the design's non-goals say. The design lives at `docs/design/261105/kernel-test-framework.md`.

## Decisions

All are decided on 2026-11-05; the options, reasons and the user's choices are in the Decisions table of `docs/design/261105/kernel-test-framework.md`. None is open.

## Steps

Later steps are written when each one is introduced to the user, not before.

1. The interface library and its host tests: `kernel/lib/ktest`, with nothing linked into an image yet.
   - `kernel/lib/ktest/include/ktest/{ktest.hpp,assertions.hpp,filter.hpp}`. `KTEST(Suite, Name)` emits the body function, the one-line adapter that matches hsien's `void (*)(hsien::Context &)` invoke, and one `hsien::TestRecord` in `.hsien.tests`. `EXPECT_*`/`ASSERT_*`/`ADD_FAILURE`/`FAIL`/`KTEST_SKIP` format a message into a fixed stack buffer and call the running `hsien::Context::check`, with `ASSERT_*` adding a `return;`; the filter matcher is the gtest `*`/`?`/`:`/`-` glob over `Suite.Name`, header-only and free of kernel headers so it compiles on the host.
   - `kernel/lib/ktest/src/runner.cpp`, holding the runner, the failure reporter and the `ktest` `GKTL_COMMAND`: walk `hsien::section_begin()`/`hsien::section_end()`, select, call `hsien::run` one record at a time, and print the `RUN`/`OK`/`FAILED`/`SKIP`/`SUMMARY` lines.
   - `kernel/kernel.ld`: move the existing `.hsien.tests` block from `.init.rodata` to `.data.rel.ro`, keeping its `KEEP` and its `__hsien_tests_start`/`__hsien_tests_end` symbols. This is the one change on the runtime side, and it is what lets the runner read the records after init has been reclaimed.
   - `kernel/lib/ktest/BUILD.gn`: a `cpp_interface("headers")`, the kernel `source_set("ktest")` (`testonly = true`) depending on `//system/lib/hsien:hsien`, `//kernel/lib/gktl:headers` and `//system/lib/logz4:headers`, a host `filter-test` gtest, and a `group("tests")` wired into the root `//:default` as `//kernel/lib/ktest:tests($host_toolchain)`.
   - Why now: every later step builds on the macro/record/assertion contract, and this step is verifiable without booting: the host tests cover matching, formatting and counting, and the library compiles in the dbg.1 kernel toolchain. It touches no boot path, so a mistake here cannot break `verify`.
   - Verification: `python3 .agents/skills/verify/scripts/boot_check.py` (the host-tests line runs the new gtest and both images still boot); `prebuilt/ninja/ninja -C out/bringup.x86 kernel_x86.dbg.1/phony/kernel/lib/ktest/ktest` (the source set compiles in the kernel toolchain; the link is checked in the next step, when a record exists); `cpp-write-and-verify` (clang-format and clang-tidy) on the changed C++, `scripts/format_gn.py` on the BUILD files, `scripts/lint_docs.py --changed` on the documents.
   - Lands as one commit, `feat(ktest): the kernel test interface library on the hsien runtime`.

## Files

- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/include/ktest/ktest.hpp`, `kernel/lib/ktest/include/ktest/assertions.hpp`, `kernel/lib/ktest/include/ktest/filter.hpp`, `kernel/lib/ktest/src/runner.cpp`, `kernel/lib/ktest/test/filter_test.cpp`: new.
- `kernel/kernel.ld`: the `.hsien.tests` block moves between output sections.
- `BUILD.gn` (root): one line adding `//kernel/lib/ktest:tests($host_toolchain)` to `//:default`.
- `docs/changes/261105/kernel-test-framework.md`: the change record for this step.

## Test plan

- Host gtest: the filter grammar (`*`, `?`, `:`, `-`, an empty positive half, no match, match all), the failure-message formatting (a printable value, a value with no `std::formatter`, and user format arguments), and the counting the runner reports (checks made versus failed).
- Kernel toolchain: the source set builds under `kernel_x86.dbg.1` — the switchset the dbg image uses — with the kernel's `-fno-exceptions -fno-rtti` and its freestanding libc++ headers.
- Boot: `boot_check.py` passes and the boot lines are unchanged, because no image contains the library yet.
- The section's placement in `.data.rel.ro` is checked on the unstripped ELF in the next step, when the first record exists; this step checks that `kernel.ld` still links.
