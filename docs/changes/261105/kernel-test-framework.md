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

## The first kernel test records land, and the test-capable kernel moves to the dbg image (2026-11-05)

**Files**
- `kernel/tests/kernel_test.gni` — new: the `kernel_test()` template (a testonly source set whose include surface is the interface's headers).
- `kernel/tests/ktest_test.cpp` — new: the interface's three smoke tests.
- `kernel/tests/BUILD.gn` — the `framework` target, and the aggregate that `vmours.with-tests` links.
- `kernel/products/image/dev.dbg_1/BUILD.gn` — packages `vmours.with-tests` built in dbg.1; `kernel_package` and the image are `testonly`.
- `kernel/products/image/dev/BUILD.gn` — packages plain `vmours(dbg.0)`; its kernel package is no longer testonly.
- `kernel/products/bin/BUILD.gn`, `kernel/kernel_image.gni`, `products/bringup.gni`, `products/README.md`, `kernel/lib/lockup_detector/README.md`, `tools/gen_debug_menu.py`, `.agents/skills/verify/SKILL.md`, `docs/development/debugging.md` — every comment and document that named the image/kernel pair now names the opposite one.
- `kernel/lib/ktest/include/ktest/{assertions,ktest}.hpp` — the macro fixes and the `UPPER_CASE` renames the first real test forced.

**Symptom**

After the interface landed, no image carried a test it could run. The kernel that links `kernel.tests` (`vmours.with-tests`) was packaged by `kernel.dev.obi`, whose cmdline sets `kernel.shell=false`, so its `ktest` command could never be typed; the image that does start the kernel shell (`kernel.dev.dbg_1.obi`) packaged `vmours`, which links no test code. The product's own comments had the pair backwards too ("dev — the kernel with unit tests", "dbg_1 — without"), so the intended arrangement had never been booted.

**Cause**

The pair was left over from the intent that the test runner runs at boot inside the dev image — the July hsien design's init-hook runner, which is also why its records were placed in reclaimable `.init.rodata`. Once the decision is "tests run on demand from the kernel shell, under DEBUG_ASSERT" (2026-11-05), the only image that can run them is the debug one: it has `kernel.shell=true` and `KERNEL_DEBUG_LEVEL=1`, so a broken invariant fails a test instead of passing silently in a build whose assertions are compiled out. The `vmours.with-tests` label already existed and was already linked to `kernel.tests`; only the image edges were crossed, and a label nothing could invoke was the visible shape of that.

**Change**

The image edges, as they were and as they are:

```text
 kernel/products/image/dev.dbg_1/BUILD.gn
- "//kernel/products/bin:vmours(//build/switch/set/dbg.1:kernel_${target_cpu}.dbg.1...)"
+ "//kernel/products/bin:vmours.with-tests(//build/switch/set/dbg.1:kernel_${target_cpu}.dbg.1...)"
+ testonly = true   (on the kernel package and the image)

 kernel/products/image/dev/BUILD.gn
- "//kernel/products/bin:vmours.with-tests(//build/switch/set/dbg.0:kernel_${target_cpu}.dbg.0)"
+ "//kernel/products/bin:vmours(//build/switch/set/dbg.0:kernel_${target_cpu}.dbg.0)"
- testonly = true   (on the kernel package)
```

`kernel_test("name")` gives the tests one declaration point: a testonly source set that carries the interface's headers, aggregated by `kernel.tests`. The first tests are the interface's own smoke tests, so the image always carries at least one real record and a booted run exercises the declaration macro, both assertion families and the skip state: `Ktest.AssertionsPass`, `Ktest.SkipIsReported` and `Ktest.Names_MayHaveUnderscores` (the last one proves a test name may contain `_`, unlike a console command name).

The `KTEST` macro needed one fix, found by the first build of the first test. A macro parameter named `name` substitutes into the designator `.name`, so the record came out as `.AssertionsPass = "Ktest" "." "AssertionsPass"`; and the closing brace of the macro's anonymous namespace sat before the body function, leaving `void ktest_body_17() } {`. The parameters are now `suite_token`/`name_token`/`line_number`, and the namespace closes before the body definition. The lesson is the one every macro in this tree pays for: a macro parameter is a token name, and token names appear in places the author is not thinking about. `clang-tidy` then rejected the internal macro names because `readability-identifier-naming` wants `UPPER_CASE` and a trailing `_` is not that, so they are `KTEST_EXPECT_TRUE` and friends now.

`scripts/format_gn.py` also refused `kernel/products/bin/BUILD.gn` as unsafe: its two `import()` lines were not in gn's order, which the formatter cannot fix without reordering tokens. They were sorted as part of the edit, and the file formats clean.

**Reference**
- Zircon: `zircon/kernel/kernel_image.gni` packages the kernel variant a switchset builds, and `zircon/kernel/kernel_switchset.gni` carries `unit_tests_enabled`; the test-carrying kernel is the one whose image starts the console that runs `run_unittests` (`k ut`, `zircon/kernel/lib/unittest/console.rs`).
- GoogleTest: none for this half; the smoke tests only use `TEST`-shaped declarations, and the image does not care which framework declared them.
- This project's own design: `docs/design/261105/kernel-test-framework.md`, "Build and image integration" and its two-image table.

**Verification**
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok; dbg PASS, shell finished at 6.1 s, 46 passed / 5 skipped / 0 failed, no error line; rel PASS, shell finished at 5.2 s, 44 passed / 8 skipped / 0 failed, 7/7 required; host-tests 173 passed / 0 failed.
- `ktest list` on the dbg image prints the three `ktest: TEST` lines above.
- `ktest all` prints `RUN`/`OK` for two tests, `SKIP` for one, and `ktest: SUMMARY ran=3 passed=2 failed=0 skipped=1`; `kshell_run.py` exits 0.
- `ktest Ktest.Assertions*` prints one `RUN`/`OK` pair and `SUMMARY ran=1 passed=1`, exit 0.
- Error path: `ktest NoSuch.*` prints `ktest: ERROR no test matches "NoSuch.*"` and `SUMMARY ran=0`, and `kshell_run.py` exits 1 on the error line. This is the generic judge, before the strict `ktest` verdict exists in the next step.
- ELF: the dbg kernel (dbg.1, `vmours.with-tests`) has `__hsien_tests_start = 0x159558` and `__hsien_tests_end = 0x1595a0` inside `.data.rel.ro` [0x159000, +0x1200), which is 0x48 bytes: three 24-byte records, with the three `ktest_record_*` symbols visible. The rel kernel (dbg.0, `vmours`) has none of them. `kernel.dev.debug.json` names dbg.0/`vmours`; `kernel.dev.dbg_1.debug.json` names dbg.1/`vmours.with-tests`.
- `clang-tidy -p out/bringup.x86` on the changed C++: no diagnostic in the changed files. `format_cpp.py --check`, `format_gn.py --check` and `lint_docs.py --changed` clean; `gn check` OK on the test target and on `kernel.tests`.
- Not measured: the image-size and boot-time cost of three records (72 bytes) and the tests' code; nothing in the boot path runs them.

**Deliberately not done**
- `scripts/kshell_run.py` does not know the `ktest` grammar yet; its strict verdict (summary present, `failed=0`, `ran` equal to the listed count) is the next step.
- The three-test pilot migration (`samplerclaim.test`, `semaphore.test`, `vma_wx.test`) and the other 43 commands are untouched.
- The product-level `unit_tests_enabled` metadata still says `false`: it is informational, the linkage is the gate, and the design decided not to wire it.

**How it was found**

The first build of the first test failed with four errors, all inside the `KTEST` expansion. Reading the preprocessed translation unit (the compile database's own command with `-E`, run from the build directory) showed both mistakes in one line: `.AssertionsPass = "Ktest" "." "AssertionsPass"` and `void ktest_body_17() } {`.

**The user's prediction**

None on this step.

**Three questions you should be able to answer**

1. Why is the test-capable kernel packaged by the dbg image rather than the dev image, and which two properties of that image make it the right one?
2. Both images still link `//kernel/commands`, the 46 `*.test` commands included, although only dbg starts the kernel shell. Which switch decides that, and why is it a build argument when it is the cmdline's `kernel.shell` that decides whether the shell runs?
3. Why is a test record a 24-byte constant in `.data.rel.ro` rather than something a test registers when it starts, and what would "register at run time" cost?

## The host learns the kernel test runner's grammar (2026-11-05)

**Files**
- `scripts/kshell_run.py` — the `ktest` classification, the summary parser, the list and run judges, the setup list a run of everything is checked against, the hang diagnosis, and the module docstring.
- `scripts/test_kshell_run.py` — new: 17 unit tests over the pure verdict logic.
- `.agents/skills/verify/SKILL.md` — the "Kernel tests" section names the `ktest` command and the script's verdict.

**Symptom**

After the first records landed, `kshell_run.py` judged `ktest all` with its generic rule: no FAILED or error line, and the prompt back. That cannot tell a complete run from a partial one. If one of the three tests stopped being linked, the run would print a smaller SUMMARY -- or, before the runner's error path existed, none at all -- and still pass; the summary was never parsed. A `ktest` command that hangs was not watched either: after boot the console's silence counts as a hang only for `lockup spin all`, so the command would wait out `--command-timeout` with no NMI and no lockup report.

**Cause**

The generic rule was written for the console-command tests, whose grammar is "print `<name>: ok` and return to the prompt". A `ktest` run ends with something stronger -- a summary line carrying the three counts -- and the host's job is to check those numbers, not the presence of output. Treating the run as "any other command" discarded exactly the information the runner added for the host. The hang side is the same mismatch: a `ktest` command runs arbitrary test code with no in-kernel timeout, so the only diagnosis for a hang is the NMI and the lockup report -- machinery that was enabled for the one command known to stop every CPU, not for the command that can run anything.

**Change**

`kshell_run.py` now classifies a command by its first word, and then:

- A run (`ktest`, `ktest all`, `ktest <filter>`) ends at its `ktest: SUMMARY` line, and the line is kept (`through=True`) so it is printed and parsed. Exactly one summary must be present, `failed` must be 0, and `ran` must be non-zero.
- For `ktest` and `ktest all` the script first types `ktest list` and requires `ran` to equal the number of `ktest: TEST` lines. A test that stops being linked in then fails the run instead of disappearing from it silently.
- A filter run has no expected count, because the host does not re-implement the selection grammar; it only requires `ran > 0`.
- `ktest list` (and `?`) passes when the prompt returns with at least one TEST line and no error line.
- Any remaining FAILED or error line still fails the command, and the generic `*.test`, `crash` and `lockup` rules are untouched.
- While a `ktest` command runs, `console.silence` is the configured silence, as for `lockup spin all`: a hang is NMI'd and the lockup report is the diagnosis.
- After a `ktest` command the script drains `AFTER_VERDICT`, like a `*.test` command, so the next command is not typed into a shell still printing.

The verdict helpers are pure over the captured lines -- `is_ktest`, `ktest_lists`, `ktest_runs_all`, `ktest_summaries`, `ktest_listed`, `judge_ktest_list`, `judge_ktest_run` -- which is what lets the new unit tests run without QEMU.

Rejected: compute the expected count for a filter run on the host by globbing the listed names. That duplicates the filter grammar in a second language, where the two can drift, for a check the run-of-everything case already has; a filter that matches nothing is caught by `ran == 0` already. The glob stays in the kernel only.

**Reference**

- Zircon: `run_unittests` in `zircon/kernel/lib/unittest/console.rs` prints a per-test `PASSED`/`FAILED` and a `SUMMARY: Ran N test case(s): M failed` line, and the host harness reads those lines the same way.
- GoogleTest: the test binary's exit code is the verdict (`UnitTest::Run` answers 0 or 1). A kernel cannot exit, so the summary line carries the same contract and the host turns it back into an exit code.
- This project: `docs/design/261105/kernel-test-framework.md`, "Host side".

**Verification**

- `python3 scripts/test_kshell_run.py`: 17 tests, all passing -- classification, summary parsing, list and run verdicts, missing and duplicated summary, count mismatch, a failed test, an error line beside a passing summary, and the untouched legacy `*.test` rule.
- Booted runs on `out/bringup.x86`:
  - `ktest all`: the setup list reports `3 test(s)`; the run prints two `OK`, one `SKIP` and `SUMMARY ran=3 passed=2 failed=0 skipped=1`; the verdict is `ktest all: ok, 3 ran, 2 passed, 1 skipped`; exit 0.
  - `ktest Ktest.Assertions*`: `SUMMARY ran=1 passed=1 failed=0 skipped=0`, verdict ok, exit 0.
  - `ktest NoSuch.*`: the `ERROR` line, `SUMMARY ran=0`, verdict `FAIL, SUMMARY says no test ran`, exit 1.
  - `ktest list`: `ok, 3 test(s)`, exit 0.
  - `samplerclaim.test`: unchanged, `ok`, exit 0.
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok; dbg PASS (46/5/0), rel PASS (44/8/0, 7/7 required); host-tests 173 passed / 0 failed.
- Not verified: the hang path end to end, which would need a deliberately hanging test; its branches are the existing NMI/lockup ones, and the `ktest` runs in this step never hang.

**Deliberately not done**

- No new runner script and no `verify` integration: the same `kshell_run.py` invocation is the workflow, as the design decided.
- The filter grammar is not duplicated on the host (see Change).
- `scripts/cov_kernel.py` still enumerates only `*.test` commands; it learns the test registry in the pilot step.
- No parsing of per-test timings, and no JSON output.

**How it was found**

None; step 2's runs already printed the counts that the host did not read.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. Why does `ktest all` need a `ktest list` first while `ktest <filter>` does not, and which class of bug does the count check catch?
2. Why does the host still fail a `ktest` run on an error-level line even when the summary says `failed=0`?
3. A hung `ktest` command never returns a summary. What does the NMI/lockup path produce, and why is it a better diagnosis than the command timeout alone?

## The three-test pilot migrates, and coverage learns to find tests (2026-11-05)

**Files**
- `kernel/tests/sampler_claim_test.cpp`, `kernel/tests/semaphore_test.cpp`, `kernel/tests/vma_wx_test.cpp` — moved from `kernel/commands/` and converted from console commands to `KTEST` tests.
- `kernel/commands/BUILD.gn`, `kernel/tests/BUILD.gn` — the three commands leave the command library; the three tests get `kernel_test()` targets and the header dependencies they reach.
- `kernel/commands/user_copy_test.cpp`, `kernel/commands/mutex_stress.cpp` — the comments that said there is no in-kernel test runtime now say the test has not migrated.
- `kernel/lib/ktest/include/ktest/format.hpp` — `finish()` is `inline` (the ODR fix below).
- `scripts/cov_kernel.py`, `scripts/test_cov_kernel.py` — the coverage run asks the booted kernel for its tests and runs them; the dbg kernel ELF is `vmours.with-tests`; the new list parser is unit-tested.
- `docs/repairs/261105/kernel-test-framework.md` — new: the remaining 43 console commands, with the three that take arguments named.

**Symptom**

The interface had ten tests, but only the three smoke tests were real work: every other kernel test in the tree was still a console command with its own reporting. Two consumers still assumed that: `scripts/cov_kernel.py` enumerated only `GKTL_COMMAND`-registered `*.test` commands, so a migrated test would silently leave the coverage run, and its `KERNEL_ELF` still named the dbg image's old kernel (`vmours`, not `vmours.with-tests`).

**Cause**

Nothing was broken; this is the design's v1 measurement. The migrated tests are the same checks as the commands they replace, so the step's value is showing the interface can carry them without losing one, and finding out what a real test needs that the smoke tests did not. Two things showed up:

- A header function that is not `inline`: `ktest::internal::finish()` was a plain function in `format.hpp`. The host test had one translation unit including that header; the three kernel test translation units are the first link with three, and it failed with `multiple definition of 'ktest::internal::finish(...)'`. The header is shared by every test file, so the function has to be `inline` (or a template, as its neighbours are).
- Sign-compare on untyped literals: `EXPECT_EQ(sema.count(), 0)` compares `u64` with `int`, and the kernel builds with `-Werror`; the literal has to say what it is, `u64{0}`. The old `verdict(count == 0, ...)` expressions had the same comparison but the compiler could see the promoted context differently; the assertion macro captures both sides as `auto const &`, which makes the types explicit.

**Change**

Per file:

- `sampler_claim_test.cpp` (108 lines): one `KTEST(SamplerClaim, HoldsClaimUntilMemoryIsGone)`. The "another session holds the claim" path is `KTEST_SKIP`, the probe session and the session under test are made and closed in their own scopes (closing the last handle is what releases them), and the three checks are `ASSERT_OK`, `EXPECT_TRUE(make(&second) == Status::AlreadyExists)` and `EXPECT_OK`.
- `semaphore_test.cpp` (318 lines): five tests, one per aspect — `BanksPostsWithNoWaiter`, `PostHandsUnitToAParkedWaiter`, `TimedWaitTimesOutAndLeavesTheQueue`, `PostFromTimerHandlerWakesAParkedWaiter`, `EachPostWakesExactlyOne`. The helpers (`poll_until`, `start`, `join`, `Handoff`, `IrqCase`, `Crowd`) are unchanged; allocation and spawn paths became `ASSERT_NONNULL`, the two join-failure paths `FAIL(...)` where the command deliberately leaked, and the timed wait passes its elapsed microseconds into the assertion message. The worker threads still only publish counters for the test thread; no assertion runs on a worker thread.
- `vma_wx_test.cpp` (108 lines): one `KTEST(VmaWx, MapRefusesWriteAndExecute)`. Address-space creation is `ASSERT_OK`; the region-and-VMO setup keeps its single `EXPECT_TRUE(made)` so the map checks are skipped when setup failed; the three map checks and the destroy are `EXPECT_*`/`EXPECT_OK`.
- Each file lost its `Tally`, its `verdict()`, its final `ok`/`FAILED` line and its `GKTL_COMMAND`, and its "why a console command" comment became "why an in-kernel test".
- `scripts/cov_kernel.py`: a probe boot types `ktest list` and turns the names into `ktest <name>` commands; the run then types those, then the remaining `*.test` from the registrations. A `ktest` command's completion is its summary line with `failed=0`. The dbg kernel ELF constant follows the image swap (`vmours.with-tests`). The list parser is a pure helper, `ktest_commands(text)`, covered by `scripts/test_cov_kernel.py`.

**Reference**

- The interface's own design, `docs/design/261105/kernel-test-framework.md`, "Pilot migration".
- Zircon's kernel framework has the same shape: a test is a function, a suite is a naming prefix, and the runner owns the reporting. The `Tally`/`verdict` the migrated files carried was a local copy of what the `EXPECT` family already does.
- GoogleTest: the equivalent move is from a `main()` that prints its own verdict to `TEST`/`EXPECT`.

**Verification**

- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok; dbg PASS (46/5/0), rel PASS (44/8/0, 7/7 required); host-tests 173 passed / 0 failed.
- `ktest list` on the dbg image lists ten tests: the three `Ktest.*`, `SamplerClaim.HoldsClaimUntilMemoryIsGone`, the five `Semaphore.*`, and `VmaWx.MapRefusesWriteAndExecute`.
- `ktest all`: `SUMMARY ran=10 passed=9 failed=0 skipped=1`, verdict `ok, 10 ran, 9 passed, 1 skipped`, exit 0. The skip is `Ktest.SkipIsReported`.
- Per migration, all exit 0: `ktest SamplerClaim.*` ran 1 (45.7 ms), `ktest VmaWx.*` ran 1 (859 us), `ktest Semaphore.*` ran 5 (434 us, 1.7 ms, 5.6 ms, 2.0 ms, 26.5 ms).
- The old command names are gone: `samplerclaim.test` now prints nothing and fails after the command timeout. That is the expected user-visible cost: the invocation changes from `<name>.test` to `ktest Suite.Name`.
- `python3 scripts/test_cov_kernel.py`: 19 tests, all passing (the two new `ktest_commands` cases included).
- Coverage: `python3 scripts/cov_kernel.py --out out/cov-kernel` exited 0 after running the ten interface tests and the 43 remaining `*.test` commands in the coverage variant. The dbg kernel's report is 74.4% lines (42,153 of 56,631), with `kernel/lib/ktest` at 42.3% (142/336), `system/lib/hsien` at 52.5% (21/40), and the areas the migrated tests reach at `kernel/lib/sampler` 80.4%, `kernel/object` 90.3%, `kernel/mm` 82.0%, `kernel/kernel/sync` 86.1%. This is not like-for-like with the design's earlier 68.1% (26,894 of 39,491 on 2026-10-09): the tree and the denominator grew in between. Test sources themselves are not in the default report (`--all` counts them), so `kernel/tests/` has no row here.
- `clang-tidy -p out/bringup.x86` on the three migrated files: no diagnostic. Format, GN layout and docs lint clean; `gn check` OK on the three `kernel_test()` targets and the aggregate.
- Not measured: the wall time of `ktest all` against the old three-command sequence; the console is the same either way and the set was never run together before.

**Deliberately not done**

- The other 43 commands stay console commands; `docs/repairs/261105/kernel-test-framework.md` is the list, and the three that take arguments are called out there because the interface has no per-run argument mechanism yet.
- One `KTEST` per original command body, except the semaphore's five aspects, which were already five separate functions. Splitting `vmo_snapshot`-sized commands belongs to their own migration.
- The host verdict's legacy `*.test` path and `cov_kernel.py`'s registration scrape stay: the unmigrated commands still need both.
- No performance measurement; this is not a performance change.

**How it was found**

None; the two problems came from the first build and link of the migrated tests — `multiple definition of 'ktest::internal::finish(...)'` pointed at the header, and `-Wsign-compare` pointed at `EXPECT_EQ(sema.count(), 0)`.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. The migrated semaphore tests spawn threads that wait and post. Why does none of them call `EXPECT_*` from a worker thread, and what does the interface do if one does?
2. Why does `cov_kernel.py` boot once to ask for the test list and then again to run, and why is the first boot's profile not merged?
3. The migrated tests are the same checks as the commands they replaced. Name two places where the conversion turned a fatal setup path into its own named assertion, and say whether that added or lost coverage of the original behaviour.

## Review fixes: the publication is atomic, a skip reason is copied, and the host fails fast on an error (2026-11-05)

**Files**
- `kernel/lib/ktest/src/runner.cpp`, `kernel/lib/ktest/include/ktest/run_state.hpp` — the published run state and owner thread are `std::atomic`; `KTEST_SKIP`'s reason is copied into the state instead of pointed at.
- `kernel/lib/ktest/include/ktest/assertions.hpp` — the owner check runs before the state is dereferenced; the comparison macros say why two C strings compare as pointers (and `KTEST_SKIP`'s comment no longer asks for a literal).
- `kernel/lib/ktest/include/ktest/format.hpp` — `make_buffer`'s precondition (a non-zero room) is written down.
- `scripts/kshell_run.py`, `scripts/cov_kernel.py`, `scripts/test_kshell_run.py` — a run's end is `ktest: SUMMARY` **or** `ktest: ERROR`; the summary's counts must add up; both are unit-tested.
- `kernel/lib/ktest/README.md` — the host test path was missing `test/`, and the C-string comparison difference is now stated.
- `.agents/tasks/kernel-test-framework/REVIEW.md` — the self-review these came from; this entry records what was fixed.

**Symptom**

Latent in all four cases; each needs a condition the current tests do not create:

- A worker thread that calls `EXPECT_*` by mistake reads `CURRENT_STATE`/`OWNER_THREAD` while the runner thread writes them — a C++ data race, in the code whose job is to diagnose exactly that mistake. The design promised a file-static atomic (`docs/design/261105/kernel-test-framework.md`, "Objects"); the implementation had plain pointers.
- `KTEST_SKIP(reason)` kept the caller's pointer and the runner printed it after the body returned: a non-literal reason (a stack buffer) is a use-after-free at the SKIP line.
- A `ktest` command whose registry cannot start a run (no tests linked in, a duplicate name, three arguments) prints `ktest: ERROR` and returns without a summary. The host waited for the summary until `--command-timeout` (120 s), and because a `ktest` command keeps the silence monitor on, the machine was NMI'd and the lockup report was the diagnosis of a registry error.
- `judge_ktest_run` read the four summary numbers independently, so a `SUMMARY` whose counts did not add up would have passed the host.

**Cause**

Each is a small drift from the design or from the assumption the code was written under. The atomic was in the design and not in the implementation. The skip reason was documented as "must be a literal" instead of made safe, which a test framework cannot enforce on its own API. The host's grammar knew only the success path's terminal line, not the registry's error path. The arithmetic was never asked for. The review's items H1, H2, M2, M3 and m1 in `REVIEW.md` say the same with the reasoning; this entry is their fix.

**Change**

The publication, before and after (`kernel/lib/ktest/src/runner.cpp`):

```text
-    internal::RunState *CURRENT_STATE = nullptr;
-    ::ours::task::Thread *OWNER_THREAD = nullptr;
+    std::atomic<internal::RunState *> CURRENT_STATE{nullptr};
+    std::atomic<::ours::task::Thread *> OWNER_THREAD{nullptr};
```

with relaxed loads and stores: the check only asks "is it null, and is it this thread", so no ordering is needed between the publication and anything else; what the atomic buys is that the mistaken read is no longer a race. `record_failure` now checks `on_test_thread()` before it dereferences the state, because the state lives on the runner's stack — the worker must be told off, not read.

The skip reason, `RunState` now carries `char skip_reason[SKIP_REASON_ROOM]{}` (96 bytes) and `record_skip` copies into it, truncated; the runner prints the copy after the body returns. The comment that asked callers for a literal is gone.

The host: `KTEST_END` matches `ktest: (?:SUMMARY|ERROR)`, `judge_ktest_run` reports "no SUMMARY; the run reported: <error line>" when the run ended that way, and it now requires `passed + failed + skipped == ran`. `scripts/cov_kernel.py` uses the same end pattern. The README gained the correct host-test path and one paragraph on `EXPECT_EQ` and C strings; `format.hpp`'s `make_buffer` says it needs a non-zero room.

Rejected: making the pointer lifetime safe with reference counting. It would turn a test-only state into an allocated object for the benefit of code that is already a test bug, and the design chose the contract instead; the atomic and the check-before-dereference make the misuse safe to *report*, not safe to *do*.

**Reference**

- Zircon's kernel runner has the same shape of publication: the per-test context is passed by reference to the test function (`kernel/lib/unittest/console.rs` builds it around each invocation), so there is no global to race. This interface needs one because the assertion macros take no context argument, which is what GoogleTest also hides behind its thread-local current-test pointer (`gtest.cc` `UnitTestImpl::current_test_`).
- The host-side terminal-line grammar follows `run_unittests`' `SUMMARY: Ran N test case(s): M failed` line and its use by a harness.
- `cppreference.com/w/cpp/atomic/atomic` — relaxed operations and what they do and do not order.

**Verification**

- The skip-reason fix was verified against the failure it fixes: `Ktest.SkipIsReported` was temporarily changed to skip with `char reason[] = "..."; KTEST_SKIP(reason);`, and the booted run printed `ktest: SKIP Ktest.SkipIsReported: a reason from a stack buffer, gone when this body returns`. The file was restored, and the final boot log is the committed one.
- The host error path: `ktest NoSuch.*` now ends at the error line in 7.4 s (it used to wait 120 s), with `FAIL, no "ktest: SUMMARY" line; the run reported: ... ktest: ERROR no test matches "NoSuch.*"`, exit 1.
- `python3 scripts/test_kshell_run.py`: 20 tests (three new: the end pattern, an error before the summary, an inconsistent summary); all pass. `python3 scripts/test_cov_kernel.py`: 19 tests, all pass.
- `boot_check.py`: build ok; dbg PASS 46/5/0, rel PASS 44/8/0 with 7/7 required; host-tests 173/0.
- `ktest all`: `SUMMARY ran=10 passed=9 failed=0 skipped=1`, exit 0, with the SKIP line carrying the copied reason.
- clang-tidy on `runner.cpp` and the test translation units: no diagnostic; format, GN and docs lint clean.
- Not verified at run time: the wrong-thread assertion path (H1). Reproducing it means adding a test that violates the contract on purpose; the fix's effect is the absence of a data race by construction, not an observable change.

**Deliberately not done**

- The lifetime contract itself: a worker that asserts after its test has returned can still reach a destroyed state in principle; the API says assertions are for the test thread, and the check makes the common mistake reportable. Reference-counting the state was rejected above.
- GoogleTest's C-string special-casing in `EXPECT_EQ`: the difference is documented rather than implemented; a `PrintTo`/`FormatForComparison` increment is where it belongs.
- `products/bringup.gni`'s `unit_tests_enabled = false` against the test-capable dbg image: still the design's decision 10 (linkage is the gate), and the review lists it as a decision, not an edit.
- `make_buffer`'s zero-room case: documented as a precondition rather than made safe, because it is internal and no caller can reach it.

**How it was found**

A self-review of the whole change against `cpp-code-review` and the design document: the atomic requirement was in the design's "Objects" and not in the code; the skip reason's lifetime followed from reading `record_skip` and the runner's print order; the host wait came from reading the runner's early returns; the arithmetic from reading the judge. See `REVIEW.md` for the full report, including the items left open.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. Why is `std::memory_order_relaxed` enough for the publication pointer and the owner thread, and what would acquire/release add that this check does not need?
2. Why does the runner copy a skip reason into the state instead of keeping the pointer, and why can the same reasoning not be applied to the `hsien::Context` reference by reference counting it?
3. A run that prints `ktest: ERROR` and a summary still ends at the error line for the host. Why does the judge still fail the run, and what does the host do with the summary line that arrives after it stopped waiting?
