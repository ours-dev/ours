# kernel-test-framework changes

## The kernel test interface library lands on the hsien runtime, and its records move out of reclaimable memory (2026-11-05)

**Files**
- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/include/ktest/{ktest,assertions,run_state,format,filter,summary}.hpp`, `kernel/lib/ktest/src/runner.cpp`, `kernel/lib/ktest/test/{filter,format,summary}_test.cpp`, `kernel/lib/ktest/README.md` — the new library: the `KTEST` declaration, the `EXPECT_*`/`ASSERT_*`/`KTEST_SKIP` assertions, the selection grammar, the `ktest` console command, and the host tests for the parts that need no boot.
- `kernel/kernel.ld` — the `.hsien.tests` block moves from `.init.rodata` to `.data.rel.ro`, keeping its `KEEP` and its `__hsien_tests_start`/`__hsien_tests_end` symbols.
- `kernel/tests/BUILD.gn` — `kernel.tests`, the label `vmours.with-tests` links, now depends on `//kernel/lib/ktest`.
- `BUILD.gn` — `//kernel/lib/ktest:tests($host_toolchain)` joins `//:default`, so the host tests run with the rest.

**Symptom**

Latent: no in-kernel test can be declared without also being a console command. Every one of the 45 `kernel/commands/*_test.cpp` files repeats the same `Tally`/`verdict`/`ok` boilerplate, there is no way to list or select tests, and nothing runs them as a set. The scaffolding for a framework existed but could not be used: `kernel/tests:kernel.tests` was empty, and the runtime half — `system/lib/hsien` — had no declaration path and a section placed in memory the kernel gives back after start-up.

**Cause**

A kernel test had to be a command because a `GKTL_COMMAND` was the only thing the console could start, and the console was the only thing that could drive anything after boot. The runtime records were already designed (`TestRecord`, `Context::check`, `Reporter`, `run`) but the only way to produce one was the C++26 annotation `[[=hsien::test]]`, discovered by a Clang-AST scanner. The prebuilt compiler cannot parse that syntax: `prebuilt/clang/llvm-23/bin/clang++ 23.1.0-rc3` takes `-freflection` but has no `^^`, no annotations and no `<meta>`, and the scanner's flag belongs to a third-party fork, which the toolchain rule here excludes. The records were also placed in `.hsien.tests` inside `.init.rodata`, which is fine only for a runner that runs during init: `EarlyMem::reclaim_init_area()` frees the whole `.init*` tail after `init_arch()`/`init_platform()` on the labour thread, so a runner started from the kernel shell would read freed frames. Tests have to run after boot, under `DEBUG_ASSERT`, on the image with the kernel shell; so the records have to outlive the reclaim.

**Change**

The record type, the runner loop, the section bounds and the failure reporting are the runtime's, unchanged. What is new is a layer over them, and one placement change.

`KTEST(Suite, Name)` emits the body, a one-line adapter matching the runtime's `void (*)(hsien::Context &)` invoke, and one record:

```cpp
LINK_SECTION(".hsien.tests")
FORCE_USED static ::hsien::TestRecord const ktest_record_42 = {
    .name = "Semaphore" "." "PostWakesOne",
    .invoke = ktest_invoke_42,
    .location = ::std::source_location::current(),
};
```

The adapter is where the runtime and the assertions meet: it binds the runtime's `Context` to the runner's `RunState`, which the macros reach through one file-static pointer. `EXPECT_*` and `ASSERT_*` format the expression, both values and the author's `{}` message into a stack buffer and call the running `Context::check(false, location, buffer)`; `ASSERT_*` then executes `return;`. Nothing panics, nothing allocates, and no dynamic initialization is involved: the record is constant-initialized data.

The one runtime-side change, in `kernel/kernel.ld`:

```text
-    .init.rodata : AT(ADDR(.init.rodata)) {
-        /* hsien test records */
-        PROVIDE_HIDDEN(__hsien_tests_start = .);
-        KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))
-        PROVIDE_HIDDEN(__hsien_tests_end = .);
+    .data.rel.ro : ALIGN(8) {
+        ...
+        PROVIDE_HIDDEN(__hsien_tests_start = .);
+        KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))
+        PROVIDE_HIDDEN(__hsien_tests_end = .);
```

`.data.rel.ro` is where the kernel keeps read-only-after-relocation registries, beside the command registry; the walk finds them there after init memory is gone. The alternative — moving no section and running the suite during init, as the July design intended — was rejected: a panicking test would then break every boot of the image `verify` uses, the run would have no per-run selection, and the assertions-on image is not the one with `vmours.with-tests` today.

Rejected options, in short form: a new record type in `kernel/lib/ktest` (duplicates what `system/lib/hsien` already has, and the annotation scanner could never share it); registration through static constructors (the kernel does run them, but it puts the registry on the heap and makes test presence depend on init order, for nothing); one runtime per layer (the user chose reuse on 2026-11-05).

**Reference**

- Zircon kernel test framework: `zircon/kernel/lib/unittest/include/lib/unittest/unittest.h` (`UNITTEST_START_TESTCASE`, `UNITTEST`, `UNITTEST_END_TESTCASE`, `BEGIN_TEST`/`END_TEST`, `EXPECT_*`/`ASSERT_*`), the `.data.rel.ro.unittest_testcases` section, and the runner `run_unittests` in `zircon/kernel/lib/unittest/console.rs` behind `k ut`.
- GoogleTest: `TEST`/`GTEST_TEST_` in `vendor/googletest/googletest/include/gtest/gtest.h` and `gtest-internal.h` (`GTEST_FATAL_FAILURE_` returns, `GTEST_NONFATAL_FAILURE_` falls through); `EXPECT_EQ` and friends expand through `EqHelper::Compare`; the selection grammar is `UnitTestImpl::FilterTests` in `googletest/src/gtest.cc`.
- This project's own runtime: `hsien::run`, `hsien::Context::check`, `hsien::Reporter` and `hsien::section_begin()/section_end()` (`system/lib/hsien`), reused as-is. The design is `docs/design/261105/kernel-test-framework.md`.

**Verification**

- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 52.1 s; dbg PASS, shell finished at 6.3 s, 46 passed / 5 skipped / 0 failed, no error line; rel PASS, shell finished at 5.1 s, 44 passed / 8 skipped / 0 failed, 7/7 required; host-tests PASS, 173 passed / 0 failed in 9.9 s (the new executable is one of them: 9 cases across the filter, format and summary suites).
- The kernel-toolchain source set builds: `prebuilt/ninja/ninja -C out/bringup.x86 kernel_x86.dbg.0/phony/kernel/lib/ktest/ktest`.
- The section move is visible in the linked rel kernel: `__hsien_tests_start == __hsien_tests_end == 0x12e558` inside `.data.rel.ro` (0x12e000 + 0x11b8), while `.init.rodata` no longer contains `.hsien.tests`. The same image carries `hsien::run`/`section_begin`/`section_end` and the `commands::Ktest` record; the dbg image's `vmours` carries none of them (the only `kTest` substring in its symbols is `TickFallbackTest`).
- `clang-tidy -p out/bringup.x86` on the four C++ files: no diagnostic in the changed files (the ones printed are pre-existing, in `system/lib/ustl` headers); `scripts/format_cpp.py --check` clean; `scripts/format_gn.py --check` clean.
- Not measured: the image-size and boot-time cost of linking the library into the test kernel; nothing in the boot path references it yet.

**Deliberately not done**

- No tests are registered yet: the registry is empty, so `ktest` would answer "no tests are linked into this kernel". The GN interface, the first record and the image swap are the next step.
- `scripts/kshell_run.py` does not know the `ktest` grammar yet, and the 43 remaining `*.test` commands are untouched; `scripts/cov_kernel.py` still enumerates only the command registry.
- Fixtures, parameterized and typed tests, matchers, death tests, per-test isolation and timeouts stay non-goals, as the design says.
- The annotation scanner stays dormant, `system/lib/hsien`'s own sources are untouched, and the unused `UNITTESTS_ENABLED` switchset plumbing is left alone.
- The production images' kernel mapping is unchanged in this step; the with-tests kernel still belongs to the rel image, which has no shell, so the command cannot be reached there.

**How it was found**

None; this is a new feature.

**The user's prediction**

None on this step. The four decisions this builds on (reuse the hsien runtime, `EXPECT`/`ASSERT` semantics, on-demand runs on the dbg image, a three-test pilot) were made on 2026-11-05 and are recorded in the design's Decisions table.

**Three questions you should be able to answer**

1. Why must `.hsien.tests` live in `.data.rel.ro` when the July design put it in `.init.rodata`, and what exactly would a post-boot `ktest` read if it moved back?
2. `KTEST` emits an existing `hsien::TestRecord` rather than a record type of its own. What does that let a future annotation scanner reuse without touching the runtime or the runner?
3. Why does `ASSERT_*` have to be a macro that executes `return;` instead of a function, and why does that limit it to `void`-returning functions?
