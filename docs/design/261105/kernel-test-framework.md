# kernel-test-framework: a kernel test interface modeled on Zircon's ktest and GoogleTest, on the hsien runtime

**Date**: 2026-11-05
**Status**: Accepted

## Problem

The kernel has 46 `*.test` commands (45 files under `kernel/commands/*_test.cpp`, 23,338 lines, plus `idle.test` in `idle.cpp`) and one `*.stress`, and no test framework. 42 of the 45 files declare the same `struct Tally`, 41 carry a near-identical `verdict()` helper that logs `pass`/`FAIL` lines, and 42 and 39 respectively hand-write the final `<name>.test: ok -- N checks` / `<name>.test: FAILED -- N of M checks` line. A test is only runnable by being a console command, which forces its name through `gktl`'s command encoder — a name with `_` is silently unreachable (`kernel/lib/gktl/src/command.cpp:26-46`; noted in `kernel/commands/vmo_snapshot_test.cpp:37-38`) — and the console command is the only way in, so there is no list, no filter, no per-test summary and no way to run the set.

The consumers are as ad hoc as the producers. `scripts/kshell_run.py` recognises a test by its first word ending `.test` (`:657-658`), waits for `name: (ok|FAILED)` (`:666`), and fails a command on any `FAILED` or error-level line (`:409-421`). `scripts/cov_kernel.py` scrapes `GKTL_COMMAND` registrations with a regex to know which tests to run for coverage (`:78,:150-159`). The 172 host test executables (GoogleTest programs and a few plain `int main()` smokes) exercise code that also compiles for the host; none of them runs code that only exists in a booted kernel, so the scheduler, address-space and interrupt paths under test here have no automated run at all. The scale of the gap shows in the 2026-11-03 record (`docs/changes/261103/kernel-task-sched.md:623`): of the 46 `*.test`/`*.stress` commands, 8 were run in one task, 6 passed, 2 failed and 38 were not run at all — nobody can run the set, and so it is not run.

The scaffolding for a framework exists but is empty or inverted. `kernel/tests/BUILD.gn` is `source_set("kernel.tests")` with no sources; `kernel/products/bin/BUILD.gn:23-35` builds `testonly vmours.with-tests` and links that empty set, with the comment "structural until the local unittest runtime exists"; but the image that packages `vmours.with-tests` is `kernel.dev.obi` (`kernel/products/image/dev/BUILD.gn:517-529`), the release image, which runs with `kernel.shell=false` and assertions compiled out, while `kernel.dev.dbg_1.obi` — the image with the kernel shell and `KERNEL_DEBUG_LEVEL=1` (`kernel/products/image/dev.dbg_1/BUILD.gn:38,:80-100`) — packages the plain `vmours`. The switchset field `unit_tests_enabled` exists and defines `UNITTESTS_ENABLED` (`build/switch/kernel_switchset.gni:20-21,121`; `kernel/products/BUILD.gn:113-117`), but no C++ reads it, and only the unused `dbg.2` switchset sets it true (`build/switch/set/dbg.2/BUILD.gn:4-6`). Two tests' comments name the hole by pointing at `vmours.with-tests` (`kernel/commands/user_copy_test.cpp:39`, `kernel/commands/mutex_stress.cpp:25`).

The tree does have the runtime half of a framework, built and then left dormant: `system/lib/hsien` (2026-07-20, its spec at git history `56c9ed61`, `docs/superpowers/specs/2026-07-20-hsien-test-framework-design.md`), whose `TestRecord`, `Context`, `Reporter`, runner and `.hsien.tests` section are exactly the record/reporting seam a kernel test framework needs. Its *declaration* path is a C++26 annotation (`[[=hsien::test]]`) discovered by a Clang-AST scanner, and that cannot run here: `prebuilt/clang/llvm-23/bin/clang++ 23.1.0-rc3` takes `-freflection` in `--help-hidden` but parses neither `[[=test]]` nor `^^T` and has no `<meta>` header, while the scanner's `-freflection-latest` belongs to a third-party P2996 fork image, not to an official release — and the repo takes toolchains only from official upstream releases. Its records also sit in `.hsien.tests` inside `.init.rodata` (`kernel/kernel.ld:224-227`), which `EarlyMem::reclaim_init_area()` frees after start-up (`kernel/top/main.cpp:62`; `kernel/mm/early_mem.cpp:233-262`), so only a runner that runs during init could read them. The user decided on 2026-11-05 to reuse this runtime and build the missing kernel-facing interface on top of it rather than write a second runtime; the annotation scanner stays dormant, and the only runtime-side change is where the records live.

References. Zircon's in-kernel test framework is `zircon/kernel/lib/unittest` (C++: `UNITTEST_START_TESTCASE`/`UNITTEST`/`UNITTEST_END_TESTCASE`, `BEGIN_TEST`/`END_TEST`, `EXPECT_*`/`ASSERT_*`, records in `.data.rel.ro.unittest_testcases`, run by the `k ut` command), plus its 2026 Rust rewrite whose rustc cfg is `ktest` (`#[suite]` module, `#[test]` functions, `expect_*!`/`assert_*!`); Zircon has never had a `kernel/ktest` directory — "ktest" names the cfg, not a path. GoogleTest 1.18.0 (`vendor/googletest`) is the API model for the host tests and for what a test-writing API should look like. Linux's KUnit is the boot-time variant of the same idea and is mentioned under Alternatives only.

## Goals and non-goals

Goals:

- G1 One declaration. A test is declared with `KTEST(Suite, Name) { ... }` in a test source; no console command, no manual registry, no build-time code generation.
- G2 Familiar assertions. `EXPECT_*` records a failure and continues; `ASSERT_*` records a failure and returns from the current function; every failure line carries `file:line`, the expression text, both values where they can be printed, and an optional formatted message.
- G3 Suites and names. A test's stable full name is `Suite.Name`; `ktest list` lists the registry; `ktest <filter>` selects with the gtest filter grammar (`:` alternatives, `*`/`?` wildcards, `-` exclusions) over the full name.
- G4 One runner. A `ktest` console command runs a selection, prints one line per phase and a final `SUMMARY`, and `scripts/kshell_run.py` turns that into the process exit code: 0 only when the summary is present, `failed=0`, and every expected test ran.
- G5 Tests run where bugs are caught. The test kernel is the dbg image's kernel (DEBUG_ASSERT on, kernel shell on COM1); production kernels and the rel image link no test code and pay nothing.
- G6 Coexistence. The 46 existing `*.test` commands keep working unchanged while tests migrate one by one; v1 migrates a three-test pilot and files the rest in `docs/repairs/`.
- G7 The generic parts are tested on the host. The hsien runtime keeps its hosted tests, and the filter matcher and assertion formatting added here get host tests before any boot.

Non-goals:

- Host tests: they stay GoogleTest executables discovered by `scripts/host_tests.py`; no new framework for them.
- A user-space test framework: the shell's self-checks (`system/app/shell/shell_tests.cpp`) and the boot checks stay as they are.
- Fixtures (`SetUp`/`TearDown`, suite setup, global environment), parameterized tests, typed tests, matchers, shuffle, repeat, sharding.
- Death tests and kernel-fault recovery in v1: a later increment is sketched under Design ("Death tests and kernel faults"), and the reason there is no in-kernel recovery is recorded under Alternatives.
- Per-test isolation: no dedicated thread, no fresh address space, no per-test timeout, no parallel execution. A test runs on the kshell thread while the other CPUs keep running, exactly as the console-command tests do today.
- Running tests at boot, in `verify`, or from user space.
- Reviving hsien's annotation declaration path (the scanner stays dormant) or deleting hsien.

## Design

### Reference map

| Reference | Borrowed | Deliberately different here |
|---|---|---|
| GoogleTest `TEST(Suite, Name)` (`vendor/googletest/googletest/include/gtest/gtest.h:2202`; expansion `include/gtest/internal/gtest-internal.h:1484-1521`) | one macro per test, identifier arguments, implicit registration | registration is a linker section, not a per-test static initializer (`GTEST_TEST_` needs a heap and a dynamic-init pass); no fixture class |
| GoogleTest `EXPECT_*`/`ASSERT_*` (`include/gtest/internal/gtest-internal.h:1299-1302`: fatal is `return`, non-fatal falls through) | the two-name split and its semantics | fatal is a plain `return` because the kernel builds with `-fno-exceptions` (`build/config/BUILDCONFIG.gn:190-196`); message is a format string, not `<<` streams, because the kernel has no iostreams |
| GoogleTest filter grammar (`src/gtest.cc:820-880`) | `:`-separated patterns, `*`/`?` globs, `-` exclusions over `suite.name` | one filter token from the shell, not `--gtest_filter`; a filter that matches no test is an error, not a silent pass |
| GoogleTest output tags (`[ RUN ]`/`[ OK ]`/`[ FAILED ]`/`[ PASSED ]`, `src/gtest.cc:3460-3720`) | the vocabulary and the summary line | every line starts with `ktest: ` so the host finds it in a serial log where other CPUs interleave; no per-suite headers and no environment lines |
| Zircon C++ kernel framework (`zircon/kernel/lib/unittest/include/lib/unittest/unittest.h:409-422`, `.data.rel.ro.unittest_testcases`, runner `kernel/lib/unittest/console.rs`) | one record per test in a KEEP'ed RELRO section walked between `__start`/`__stop` symbols; a console command as the only entry point; `EXPECT`/`ASSERT` names; `file:line` in failures; on-demand runs | suites are name prefixes, not a `{name, tests[], desc}` record; no `BEGIN_TEST`/`END_TEST` bookkeeping variable; no per-suite thread or user aspace; no online-CPU-mask invariant check (our hotplug tests change it on purpose) |
| Zircon Rust `ktest` (`#[suite]` + `#[test]` + docstrings, `zircon/kernel/lib/unittest/{src/lib.rs,unittest-macro/src/lib.rs}`) | the `ktest` name, stable test names, and the "suite is cosmetic, test is the unit" model | no docstrings: C++ has no attribute our compiler can read once annotations are out, and a description string with no reader is noise; suite and test are two macro identifiers instead of a module and a function |
| Zircon `k ut [case|all|?] [-r num]` (`console.rs:54-66`) | `all` and `?`/`list`, a summary line, a fail count | `ktest` is the command, not `ut`; selection is a filter over names, not one exact suite name |
| Zircon `kernel_with_tests` switchset and `kernel_test_deps.gni` | a test kernel distinct from the production kernel | our test code is gated by the link edge (`vmours.with-tests` → `kernel.tests`), not by a define, because no production source needs test-only branches |

### Objects

- `hsien::TestRecord` — reused as the registry element, unchanged: `{char const *name; void (*invoke)(hsien::Context &); std::source_location location}`, one per test. The macro produces instances of this type; no new record type is introduced.
- `.hsien.tests` — the reused section, with the `__hsien_tests_start`/`__hsien_tests_end` symbols `system/lib/hsien/src/section.cpp:5-14` already reads. It moves from `.init.rodata` to `.data.rel.ro` (beside the command registry), which is the only runtime-side change: the runner reads it on demand, long after `EarlyMem::reclaim_init_area()` has handed the init frames back. `KEEP` stays, for the reason every registry section carries it.
- `hsien::Context` — reused unchanged: current test name, failure count, `Reporter`, and `check(condition, location, message)` which increments the count and reports. The assertion layer formats its message into a stack buffer and calls `check(false, ...)`; that is the whole integration.
- `hsien::Reporter` — reused unchanged: one report callback. The kernel side implements it to print `ktest: FAIL ...` from the `hsien::Failure` the runtime hands over; `hsien::run(begin, end, reporter)` keeps doing the per-test failure accounting.
- `ktest::internal::RunState` — what hsien does not carry, and all that the kernel-facing layer adds to per-test state: a pointer to the running `hsien::Context`, the owner thread, the number of checks made, and a skipped flag. The runner owns one, the generated adapter binds it to the `hsien::Context` it is given, and the assertion macros reach it through one file-static atomic pointer.
- `ktest` command — one `GKTL_COMMAND` in the kernel-facing layer. It exists only in a kernel that links the tests, i.e. only `vmours.with-tests`.
- Layering: `system/lib/hsien` stays the generic freestanding runtime (its hosted tests keep running), `kernel/lib/ktest` is the kernel's interface to it — the macro, the assertions, the filter, the command and the reporting policy. This is the same split as `system/lib/concurrent` and `kernel/lib/kconcurrent` (`.agents/skills/kernel-design/references/layering.md:99-106`), and it keeps hsien free of kernel headers as that rule requires.

### Registration

`KTEST(Suite, Name)` at namespace scope expands to a body, a one-line adapter with the signature hsien's `invoke` already has, and one `hsien::TestRecord` in `.hsien.tests`; the name tokens must be identifiers, as `TEST` requires in GoogleTest. Concretely, `KTEST(SamplerClaim, ClaimsOnce)` expands to approximately:

```cpp
namespace {
void KtestBody_SamplerClaim_ClaimsOnce();

::hsien::TestRecord const ktest_record_SamplerClaim_ClaimsOnce
    LINK_SECTION(".hsien.tests") FORCE_USED = {
        .name = "SamplerClaim" "." "ClaimsOnce",
        .invoke = &KtestAdapter_SamplerClaim_ClaimsOnce,
        .location = std::source_location::current(),
};

void KtestAdapter_SamplerClaim_ClaimsOnce(::hsien::Context &hsien_context) {
    ::ktest::internal::RunState state{hsien_context};
    ::ktest::internal::Publish published(&state);
    KtestBody_SamplerClaim_ClaimsOnce();
}

void KtestBody_SamplerClaim_ClaimsOnce() {
    // the braced body the author wrote
}
} // namespace
```

Points that matter:

- hsien's `TestRecord` is the seam between declaration and runtime, exactly as its July spec described it: "the exact storage representation may be reduced for the kernel image, but the runner must not depend on Python, Clang, libc++, or `std::meta`". The macro only produces instances of that record; `hsien::run`, `Context` and `Reporter` are not modified.
- The adapter is the only thing that knows about the kernel-facing state: it binds the `hsien::Context` the runtime created to the runner's `RunState` and publishes it for the assertions. This keeps the test body `void()` like GoogleTest's, and keeps the runtime, which passes a `Context &`, satisfied.
- `LINK_SECTION`/`FORCE_USED` are the same attribute macros `GKTL_COMMAND` uses (`system/public/ours/compiler.hpp:27-28`; `kernel/lib/gktl/include/gktl/types.hpp:9-11`); the section keeps its `KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))` and its symbols, so hsien's `section.cpp` is untouched and all that changes in `kernel.kernel.ld` is which output section holds the block.
- The record is constant-initialized, so `ccrt::init_global_static_objects()` (`kernel/top/main.cpp:87`) does nothing for it. This is the deliberate divergence from GoogleTest's per-test static initializer: no heap use at boot, no dependence on constructor order, and the records are part of the image rather than of an allocated registry.
- `std::source_location` is reused from hsien's record type; it already compiles in hsien's freestanding check, and the first implementation step confirms it in the kernel toolchain, with `__FILE__`/`__LINE__` as the fallback (as Zircon's C++ framework passes).
- A duplicate `Suite.Name` is possible (two records in different anonymous namespaces both link); the runner detects it by comparing names pairwise at the start of a run and reports it as a framework error rather than running an ambiguous name. The registry is small (tens of records), so no map and no heap are needed.
- Removing tests from an image is deleting the dependency edge; no source changes and no define. An image without the layer has an empty section and `hsien::section_begin() == hsien::section_end()`.
- The annotation scanner and its generated wrapper are not used. They can return later as a second declaration front end without touching the runtime or this runner, because both would emit the same `hsien::TestRecord`; that is the interchangeability the July spec designed for, kept intact.

### Assertions

The v1 set, with GoogleTest names and Zircon's kernel additions:

- `EXPECT_TRUE`/`EXPECT_FALSE`, `ASSERT_TRUE`/`ASSERT_FALSE`.
- `EXPECT_EQ`/`EXPECT_NE`/`EXPECT_LT`/`EXPECT_LE`/`EXPECT_GT`/`EXPECT_GE` and their `ASSERT_*` twins.
- `EXPECT_NULL`/`EXPECT_NONNULL` and their `ASSERT_*` twins.
- `EXPECT_OK(expr)`/`ASSERT_OK(expr)` for `ours::Status == Status::Ok`, printing the status with the existing `to_string(Status)`; the name matches Zircon's kernel `EXPECT_OK`.
- `ADD_FAILURE("message {}", args...)` records and continues; `FAIL("message {}", args...)` records and returns.
- `KTEST_SKIP("reason")` marks the test skipped and returns; a skipped test is not a failure.
- Every assertion takes an optional trailing format string and arguments in `logz4`'s `{}` style, e.g. `EXPECT_EQ(child->size(), parent->size(), "after {} writes", n)`. The message is created only when the check fails.

Semantics:

- `EXPECT_*` formats its message when the check fails and calls the running `hsien::Context::check(false, loc, buffer)`, which increments the context's failure count and calls the reporter; then execution continues.
- `ASSERT_*` does the same and then executes `return;`. It is therefore only valid in a `void`-returning function, which the test body is; in a helper it returns from the helper, not from the test — the same caveat GoogleTest documents. This is the one deliberate departure from hsien v1's "failures are recorded and never terminate the current test" rule: a precondition failure (`ASSERT_OK`) must stop before the test dereferences a null object, or the kernel toolchain's always-on `ASSERT`/`DEBUG_ASSERT` panics and takes the whole run with it (`system/public/ours/assert.hpp:182-220`; `kernel/top/panic.cpp:226-252`).
- `KTEST_SKIP` sets the run state's skipped flag and returns; hsien has no skip concept, so the flag lives on the kernel-facing side and the runner reports it.
- Assertions never panic, sleep or longjmp; there is no `setjmp`/`longjmp` in the kernel tree anyway. hsien's own `Context::check` contract already says a failure never changes control flow, and `ASSERT_*` is only the macro's extra `return`.
- A kernel `ASSERT`, a `DEBUG_ASSERT`, a page fault or a panic inside a test is not a framework failure: the machine ends without a `SUMMARY`, and the host treats the missing summary as a failure. The panic report is the diagnosis, as it is for the console-command tests today.
- Assertions are only valid on the thread running the test (the kshell thread in practice). The run state lives on the runner's stack and a worker thread's late assertion would be a use-after-free; the macro checks `Thread::Current() == state.owner` while the test is live and reports a framework error (`ktest: ERROR ...`) instead of touching the state. Worker threads publish their results for the test thread to assert on — the existing tests already poll and verify on the test thread, so the rule costs nothing. Zircon's userspace framework `zxtest` restricts assertions to the main thread for the same class of reason.

Failure output. The reporter is the one place that writes failure lines: hsien hands it a `Failure {test_name, location, message}`, and the kernel side logs one error-level line per failed check; the runner adds one error-level line per failed test at its end:

```text
ktest: FAIL SamplerClaim.ClaimsOnce kernel/tests/sampler_claim_test.cpp:42: claimed == 1: expected 1, actual 2
ktest: FAILED SamplerClaim.ClaimsOnce (1 of 3 checks failed)
```

Values are printed with `std::format` into a fixed 64-byte stack buffer, truncated with a marker; a type with no `std::formatter` specialization prints as `<unprintable>` and the check still counts, so no test is blocked on adding a printer. The panic path already formats with `std::format` into a 512-byte stack buffer (`kernel/top/panic.cpp:156-166`), so this needs no new facility. The buffer is passed to `hsien::Context::check` as its `char const *message` and is alive for the whole synchronous `check` call, so its lifetime is exactly the reporter call.

### Runner

`ktest` is one `GKTL_COMMAND` (`kernel/lib/gktl/include/gktl/command.hpp:21-27`) in the kernel-facing library. The kshell front end splits a COM1 line into at most 16 whitespace-separated arguments and calls `gktl::run_command` (`kernel/lib/kshell/kshell/shell.cpp:44-105`); there is no quoting, so a filter must be one token.

Command grammar:

```text
ktest                    run every registered test
ktest all                same
ktest list               list the registry, one "ktest: TEST <Suite.Name>" line per test
ktest ?                  same
ktest <filter>           run the tests whose full name matches the filter
ktest <selection> [name=value ...]   the run also carries options for the tests that read them
```

Run options (added 2026-11-05, after the migration start; the user chose this over splitting the argument-taking tests, decision B of `.agents/tasks/kernel-test-migration/PLAN.md`). A token after the selection is `name=value`, or `name` alone for a presence-only flag; the runner parses at most `MAX_RUN_OPTIONS` of them and logs each as `ktest: OPTION name=value` before the first test. A test reads them with `ktest::option`, `ktest::option_usize` and `ktest::has_option`, and must pass a fallback of its own, so `ktest all` and a bare `ktest <filter>` keep the ordinary configuration; `option_usize` reports a malformed value at error level and uses the fallback, which fails the run rather than silently changing what is tested. `ktest list` takes no options. The values point into the console line and are valid only while the run lasts. This is what lets the four commands that took console arguments (`interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `threadjoin.test stress N`, `timershare.test big`) migrate as ordinary tests instead of growing a second, per-test argument path.

The filter is split on `:`; a token beginning with `-` is a negative pattern, every other token is a positive one, and no positive token means `*`. Each pattern is matched against the full `Suite.Name` with a linear glob where `*` matches any run of characters and `?` matches one. Examples: `ktest SamplerClaim.*`, `ktest Sched.*:-Sched.Slow`, `ktest Semaphore.*:Vma*`. This is GoogleTest's grammar with the token classification made per-token (`src/gtest.cc:820-880`), which is what a shell without quoting can express. The words `all`, `list` and `?` are reserved.

Events of one `ktest <selection>`:

1. Get the range from `hsien::section_begin()`/`hsien::section_end()`, the kernel-only half of hsien's runtime; walk it once for duplicate names and report a framework error before running anything.
2. For every record in section order: match it against the filter; skip non-matches silently. No match at all prints `ktest: ERROR no test matches "<filter>"` and ends with `ran=0`.
3. Print `ktest: RUN <name>` at info level; create the `RunState` and its reporter, and call `hsien::run(&record, &record + 1, reporter)`; that is the runtime's own loop, its own `Context` and its own failure accounting, used one record at a time so the runner can frame and time each test.
4. From the `hsien::Summary` and the run state, print `ktest: OK <name> (<n> us)` at info level, or `ktest: FAILED <name> (<f> of <m> checks failed)` at error level, or `ktest: SKIP <name>: <reason>` at info level. The time is `time::SteadyClock::now()` around the call, in microseconds, as the console-command tests already read that clock. `f` is `summary.failed`; `m` is the run state's check count.
5. Print `ktest: SUMMARY ran=<n> passed=<n> failed=<n> skipped=<n>` at info level and return non-zero on a failure. The shell discards a command's return value (`gktl::run_command` returns `void`), so the value exists for a later programmatic caller, not for the console.

Exact grammar the host parses (every line is through `logz4`, so it carries the logger's own prefix; the host searches the raw serial stream for the `ktest: ` substring, because another CPU's bytes can land inside a line):

```text
ktest: TEST SamplerClaim.ClaimsOnce
ktest: RUN SamplerClaim.ClaimsOnce
ktest: FAIL SamplerClaim.ClaimsOnce kernel/tests/sampler_claim_test.cpp:42: claimed == 1: expected 1, actual 2
ktest: FAILED SamplerClaim.ClaimsOnce (1 of 3 checks failed)
ktest: OK Semaphore.WakesEachWaiter (1204 us)
ktest: SKIP Span.WalksLargePages (one CPU online)
ktest: ERROR no test matches "Pmm.*"
ktest: SUMMARY ran=3 passed=2 failed=1 skipped=0
```

Properties, and why they are safe:

- Ordering is section order, hence the link order of the test objects; it is deterministic for a given target and not alphabetical.
- No locking. The section is immutable after link; the run state is on the runner's stack and only the test thread touches its fields; the console runs one command at a time (`kernel/lib/kshell/kshell/shell.cpp` reads one line and dispatches it). The publication pointer is atomic only so that a mistaken assertion from a worker thread cannot be a data race while it is being diagnosed. A file-static `bool running` guards accidental re-entry, and a future second caller (a syscall, say) would need a lock before it is allowed.
- No heap. Listing, filtering, counting and formatting use the stack and fixed buffers; hsien's runner allocates nothing either; tests themselves may allocate with `new (mm::GAF_KERNEL)` as they do today.
- Reuse, not reimplementation: the record type, the context, the failure reporting and the per-record accounting are hsien's. The kernel-facing layer adds declaration, framing, selection, formatting and the command, which is the part hsien's July design left to "the hidden runner" and never got to.
- No timeout. A hung test hangs the shell, exactly like a hung console command; Zircon's `k ut` has the same no-timeout property. The host side makes the hang diagnosable instead: a `ktest` command keeps `kshell_run.py`'s 15 s silence monitor on (today it is off after boot except for the commands that stop every CPU), so a silent run is sent an NMI and the lockup detector prints every CPU's state (`kernel/lib/lockup_detector`) before the run fails for want of a `SUMMARY`.
- No isolation and no own thread. v1 runs the body on the kshell thread, as the console commands do, so a test may sleep, spawn and join threads, create an address space and use the heap. A test still owns its own state: it must join its threads and free what it allocated before returning; a failed `ASSERT` returns with whatever the test has done so far, which is the same contract the console commands have when they return early.

### Build and image integration

`kernel/lib/ktest` is a new kernel library (scaffolded with the C++ library copier, `kernel/lib/<name>`):

- `cpp_interface("headers")` with `ktest/ktest.hpp` (the macros), `ktest/assertions.hpp` and `ktest/filter.hpp`; the headers depend on hsien's `:headers` and on nothing kernel-specific, so both the filter matcher and the formatting/assertion layer are host-testable.
- `source_set("ktest")`, `testonly = true`, with the runner, the reporter and the `GKTL_COMMAND`; `public_deps = [ ":headers" ]`, deps `//system/lib/hsien:hsien` (the runtime; its `src/section.cpp` is compiled only in the kernel environment, which is exactly the half the runner needs), `//kernel/lib/gktl:headers` for the command macro and registry, `//system/lib/logz4:headers` for output, and the kernel's freestanding `<format>` for value formatting.
- `executable("filter-test")`, `testonly = true`, a host gtest for the matcher, the formatting and their counting rules; `group("tests")` around it, added to the root `//:default` group as `//kernel/lib/ktest:tests($host_toolchain)` so `scripts/host_tests.py` and `verify` pick it up with the other host tests.
- hsien's own hosted tests (`//system/lib/hsien:test/*`) keep running unchanged; the runtime they test is not modified.

`kernel/tests` gains a small GN interface and the test sources:

- `kernel/tests/kernel_test.gni` defines `kernel_test("name")`: a `source_set`, `testonly = true`, forwarding `sources`, `deps` and `public_deps`, and depending on `//kernel/lib/ktest:headers`. This is the kernel-owned test interface the user allowed when the host-side `host_test()` template was withdrawn on 2026-10-14 (`docs/design/261014/cov.md:48,60`).
- `kernel/tests/BUILD.gn` keeps the `source_set("kernel.tests")` label that `vmours.with-tests` already links; its `deps` become the `kernel_test()` targets plus `//kernel/lib/ktest` (the runtime). v1 puts the migrated tests in `kernel/tests/*_test.cpp`, one target per area. Moving a test next to the code it tests is a later layout step, once an aggregate over subsystem `tests/` directories exists (Zircon does this with its `all_libs` walk and `kernel/lib/BUILD.gn` test groups).
- Host tests keep their existing `kernel/<dir>/test/` or `kernel/<dir>/tests/` directories and are not touched by this template.

`kernel/kernel.ld` moves the existing `.hsien.tests` block from `.init.rodata` to `.data.rel.ro`, beside the command registry, keeping its `KEEP`/`SORT_BY_ALIGNMENT` and its `__hsien_tests_start`/`__hsien_tests_end` symbols:

```text
/* hsien test records (system/lib/hsien's TestRecord). On demand, not at
 * boot: the `ktest` command reads this after init has been reclaimed, so
 * it cannot live in the .init tail. KEEP for the usual registry reason. */
PROVIDE_HIDDEN(__hsien_tests_start = .);
KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))
PROVIDE_HIDDEN(__hsien_tests_end = .);
```

The images change so that the test kernel is the image with a console and assertions:

| Image | Kernel today | Kernel after | Kernel shell | Policy |
|---|---|---|---|---|
| `kernel.dev.dbg_1.obi` (dbg) | `vmours` | `vmours.with-tests` | `kernel.shell=true` | dbg.1, `KERNEL_DEBUG_LEVEL=1` |
| `kernel.dev.obi` (rel) | `vmours.with-tests` | `vmours` | `kernel.shell=false` | dbg.0, diagnostics compiled out |

Concretely: `kernel/products/image/dev.dbg_1/BUILD.gn` takes `vmours.with-tests(//build/switch/set/dbg.1:...)` and marks its `kernel_package`/image `testonly = true` the way `kernel/products/image/dev/BUILD.gn` already does; the dev image takes plain `vmours(dbg.0)` and can drop its now-unneeded testonly. The pairing comments and documentation that name which image packages which kernel are updated in the same change: `kernel/kernel_image.gni:46-47`, `kernel/products/image/dev/BUILD.gn:523-529`, `products/bringup.gni` (whose comments describe the dev image as the kernel with unit tests and the dbg_1 image as the one without, the opposite of the new split), `kernel/lib/lockup_detector/README.md:40` (which names the rel image's ELF `vmours.with-tests`), and `tools/gen_debug_menu.py:70-72` (the dbg.0 description "unit tests linked in").

The `UNITTESTS_ENABLED` define is not used by v1: the link edge is the gate, and no production source has a test-only branch. Whether to wire it or delete the unused switchset plumbing is an open question, not a v1 need.

Why the tests do not run at boot, and why the dbg image: the kernel shell is the only way to start a command, and it lives in the dbg image; tests must run under DEBUG_ASSERT so a broken invariant fails the test instead of silently corrupting state in the rel image; and every boot of both images is a gate in `verify`, so making a test panic break a boot would make every development boot a test-failure report. On-demand also gives filters and keeps the interactive diagnosis loop. The dev image keeps no test code, so the product that boots user space is exactly as it was.

### Host side

- `scripts/kshell_run.py` learns one verdict. A command whose first word is `ktest` is done at the `ktest: SUMMARY` line; it passes iff exactly one summary line is present, `failed=0`, and `ran` is greater than zero. For `ktest all` the script first types `ktest list` and counts the `ktest: TEST` lines, then requires `ran` to equal that count — a test that disappears from the image fails the run instead of silently not running. GoogleTest treats a filter that matches nothing as a warning and exit 0; here `ran=0` is a failure. A `ktest` command also keeps the 15 s silence monitor on, so a hung test is NMI'd and the lockup report is printed before the run fails for want of a summary. `ktest list` itself is judged by the shell prompt coming back and at least one `TEST` line. The existing generic rules (`FAILED` line, error-level line) still catch a failed failure-path run, so no rule is bypassed. No new script is introduced: `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest all'` is the whole workflow.
- `scripts/cov_kernel.py` learns to enumerate the framework's tests at run time: after boot it types `ktest list`, then runs each name as `ktest <name>` and dumps coverage after each, exactly as it does per command today. The `GKTL_COMMAND` scrape stays for the not-yet-migrated `*.test` commands, so coverage does not lose anything during the migration.
- `.agents/skills/verify/scripts/boot_check.py` is unchanged in v1: it builds and boots both images and runs host tests. Running kernel tests from `verify` (an opt-in `--kernel-tests` flag, then possibly by default) is a later decision based on how long the migrated suite takes.
- Formatting rules that the host changes follow: the boot logs are raw serial with NULs, so the host reader searches bytes with `grep -a`/Python `errors="replace"` discipline; `ktest` lines are matched by substring, never by assuming line boundaries (`scripts/kshell_run.py:16-24` says why the same discipline is already needed for the prompt).

### Death tests and kernel faults (not in v1)

A test that divides by zero or dereferences a bad kernel pointer is the one failure the framework cannot report from inside the kernel. The three ways a fault reaches the kernel today:

| Where the fault comes from | What happens today | Can a test observe it? |
|---|---|---|
| Kernel-mode `#DE`/`#UD`/`#GP`, or `#PF` on a kernel address (`kernel/arch/x86/kernel/faults.cpp:145-245`) | `panic_exception()` prints the vector, error code and registers, then halts the CPU in a loop (`faults.cpp:50-71`); no panic banner and no crash record | Only as a dead machine: the register dump is already on the serial line, and the host's silence NMI makes the lockup detector print every CPU's state |
| A fault inside the user-copy window (`kernel/arch/x86/kernel/user_copy.cpp:50-72`) | the page-fault handler sees the thread's `RUN_FAULT_HANDLER` resume slot and hands the fault back to the copy routine, which returns an error | Yes — `copy_from_user`/`copy_to_user` style checks, which the existing user-copy tests already use |
| A ring-3 fault (`#DE`/`#PF`/`#UD`/`#GP` in user code) | packaged as `task::ExceptionContext` and delivered to the thread's or process's exception channel, then the process is killed (`faults.cpp:96-125,311-333`) | Yes — a kernel test can watch a user thread or process die |

So the only "catch and continue" the kernel has is the cooperative, single-point one inside the user-copy window. A kernel-mode fault has no recovery hook: the CPU is halted inside the handler, and continuing the same run would mean abandoning C++ frames without unwinding, with whatever the interrupted code held — spin locks, mutexes, a preemption count, an interrupt-off state, the user-copy state, a half-queued Dpc — left behind. The kernel has no `setjmp`/`longjmp`, lockdep is off by default, and the framework cannot prove the fault was taken in a safe point, so a "successful" recovery could leave the machine corrupted and later tests passing falsely. That is why in-handler recovery is rejected for now (Alternatives).

If death tests are wanted later, the sound shape is a host-orchestrated one with **no kernel recovery at all**:

1. `KTEST_DEATH(Suite, Name)` records go into a second section, `.ktest.death`, owned by the kernel-facing layer; the record type is still `hsien::TestRecord`, so the runtime is not touched again.
2. `ktest all` never runs them. The host's normal run covers the ordinary tests, then the host loops over the death tests.
3. One boot per death test: boot, type `ktest death <Suite.Name>`, and wait for the expected report — the `#DE Divide error` vector and register dump, or a panic banner whose message matches. A death test that leaves the machine alive fails, and so does a death whose report is not the one expected.
4. Reboot for the next death test; the kernel is gone after each one, so the "one death per boot" rule is mechanical. `scripts/kshell_run.py` already judges panic reports for the `crash` and `lockup` commands, `scripts/cov_kernel.py` already reboots after a dead machine, and `kernel.dev.dbg_1` already sets `kernel.panic.reboot=true` (`kernel/products/image/dev.dbg_1/BUILD.gn`), so this is a loop around existing machinery rather than new transport.
5. A `KTEST_EXPECT_PANIC("message")` form for a death test that expects `panic()` rather than a CPU exception can share the same flow.

What has to change before that increment: the kernel-mode exception path must reach `vpanic()` after its register dump instead of halting, so `kernel.panic.reboot=true` resets the machine deterministically and the crash record is written (`kernel/top/panic.cpp:116-123`); whether that halt is worth changing for every image is a kernel-wide decision of its own, not a test-task change. The death-test distinction rides on the section a record is in, not on a new field of `hsien::TestRecord`, which is what keeps the hsien runtime untouched.

### Test authoring rules

- One file per area under `kernel/tests/`, named `<area>_test.cpp`, with its tests in an anonymous namespace and its helpers `static`.
- Tests are independent: none may rely on another having run, on link order, or on the machine having been left in a state by an earlier test. Link order decides only what a run prints first.
- A test body runs on the kshell thread of a live, other-CPU-busy machine; it must not assume exclusive access, must join every thread it starts and free every allocation before returning, and must not call `ktest` recursively.
- Assertions are on the test thread only; worker threads must publish state for the test thread to assert on. `KTEST_SKIP` is the way to say "this machine cannot exercise it" (today's tests log an "ok" with a parenthetical instead; skip makes that machine-readable and visible in the summary).
- A test may take as long as it needs; there is no in-kernel timeout, by design. The host's silence NMI and the lockup detector are the diagnosis for a hang.
- Test names are identifiers: `KTEST(Suite, Name)` produces `Suite.Name`. This is only a naming convention for filters and output; unlike a console command name, it is not constrained by `gktl`'s encoder (`kernel/lib/gktl/src/command.cpp:26-46`), because the framework's command name is the fixed word `ktest` and test names are only ever compared as strings.

### Pilot migration

v1 converts three existing commands, chosen to exercise the framework's three needs — a plain object, threads and timeouts, and an address space:

- `kernel/commands/sampler_claim_test.cpp` (108 lines) → `kernel/tests/sampler_claim_test.cpp`, no setup, one object.
- `kernel/commands/semaphore_test.cpp` (318 lines) → `kernel/tests/semaphore_test.cpp`, threads and deadlines.
- `kernel/commands/vma_wx_test.cpp` (108 lines) → `kernel/tests/vma_wx_test.cpp`, user address space and mappings.

The migration keeps every check, replaces `Tally`/`verdict` and the final `ok`/`FAILED` lines with the macros and the summary, and deletes the `GKTL_COMMAND` registration. The other 43 `*.test` commands keep working untouched while they are migrated over time; the remaining work is filed in `docs/repairs/` in the same change (one item: "the remaining kernel `*.test` commands are still console commands and carry their own reporting boilerplate", ordered by size).

## Alternatives considered

- Keep the console commands and only extract `Tally`/`verdict`/the final line into a shared helper. It is a few hundred lines saved and no behavior change, but it leaves discovery, the grammar, the runner and the consumers exactly as they are — no list, no filter, no summary, no way to run the set, and the naming constraint of a command. Rejected: it fixes the duplication but not the gap that actually stops tests from running.
- Write a second runtime in `kernel/lib/ktest` and leave hsien alone. It would keep the kernel layer with no `system/lib` dependency and no edit to `kernel.ld`, but it duplicates the record type, the context, the failure reporting, the section symbols and the runner loop that hsien already has, and it puts two test runtimes in the tree. This is the option the user rejected on 2026-11-05: reuse hsien's runtime instead.
- Move hsien itself into `kernel/lib` and use it only from the kernel. It would make the dependency direction look simpler, but hsien's hosted tests and its freestanding/dual-environment design are the reason it lives in `system/lib`, and kernel headers are exactly what it must stay free of. Rejected.
- Revive `hsien`'s annotation discovery. Rejected for v1 because the only compiler that parses the syntax is the P2996 fork, the repo takes official prebuilt toolchains only, and a framework whose declaration syntax cannot be compiled is not a framework. The scanner also makes test discovery depend on a Python AST walk of Clang JSON, which is more moving parts than the macro for the same records.
- GoogleTest-style registration through static constructors. The kernel does run global constructors (`ccrt::init_global_static_objects()` over `.ccrt_init`), so it is technically possible; rejected because it puts the registry on the heap, makes test presence depend on dynamic-init order, and buys nothing over a KEEP'ed section whose records are already in the image.
- Boot-time auto-run, as `hsien` v1 planned with its init hook, or as Linux KUnit does. It fits the old image mapping and `.init.rodata` placement, but tests would then run before user space on every boot of the release image, a panicking test would break every boot `verify` performs, there is no per-run selection, and the dbg image, whose assertions are what catch kernel invariants, would not run them. Rejected; the dedicated test image + boot option variant is the later option if CI wants a single-shot boot-and-run.
- Catch kernel-mode faults in the handler and jump back into the runner (a `setjmp`/`longjmp`-style trap-frame rewrite, or a test thread that is killed instead of the machine). It is technically possible — the trap frame is on the thread's kernel stack and the stub restores registers from it — but the abandoned frames may hold spin locks, mutexes, a raised preemption count or an interrupt-off state, and with no unwinding, no exceptions and lockdep off by default the framework cannot know whether the fault was taken somewhere safe. A recovery that leaves a lock held turns one bad test into a hang or a false pass for every test after it, which is worse than losing the run. Rejected; the later death-test shape above keeps the kernel out of the recovery business and restarts instead.
- Zircon's per-suite thread with a fresh user address space. It isolates a suite from the console thread and from leaked user mappings, but it adds thread and address-space lifecycle to a framework whose tests are precisely about those objects, and the current commands already run on the kshell thread. Deferred as a later increment.
- GoogleTest's `<<` message style, fixtures, parameterized and typed tests, matchers, death tests and per-test timeouts. The message style is impossible without iostreams; the rest are scope beyond "basically usable" and are named as non-goals. Death tests have no kernel analogue (no fork, and a panic ends the machine); timeouts would need a watchdog seam the kernel does not have.
- An XML/JSON machine-readable result format. The serial line grammar is enough for one host consumer and stays readable at the console; a second format is a second thing to keep true.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| What happens to `system/lib/hsien` | A: leave it untouched and write a second runtime; B: reuse its runtime and build the kernel interface on top; C: retire it and use its lessons only | B | | B, 2026-11-05 |
| Failure semantics | A: GoogleTest/Zircon split — `EXPECT_*` continues and `ASSERT_*` returns from the current function; B: `hsien` v1's rule — failures are recorded and nothing ever aborts | A | | A, 2026-11-05 |
| Where tests run | A: on demand from the dbg image's kernel shell, with `vmours.with-tests` moved into `kernel.dev.dbg_1` and the rel image back on plain `vmours`; B: at boot in the dev image (today's image mapping), like Linux KUnit; C: both, A now and a dedicated boot-time test image later | A | | A, 2026-11-05 |
| v1 migration scope | A: the interface plus a three-test pilot, the rest filed in `docs/repairs/`; B: migrate all 46 `*.test` commands in this change | A | | A, 2026-11-05 |
| Framework name and API names | A: kernel interface `ktest` (`KTEST`, `EXPECT_*`, `ASSERT_*`) on the `hsien` runtime; B: rename the whole framework `hsien`; C: another name | A | | A, 2026-11-05 |
| Filter and output protocol | A: gtest-style filter and `ktest:` lines with a `SUMMARY`, parsed by `kshell_run.py`; B: keep the `<name>: ok` grammar and only unify the helpers | A | | A, 2026-11-05 |
| Failure message style | A: optional trailing format string and arguments, values printed with `std::format`; B: plain `const char *` message only, as Zircon's C++ framework | A | | A, 2026-11-05 |
| `verify` integration | A: no change in v1, kernel tests stay opt-in through `kshell_run.py`; B: `boot_check.py --kernel-tests` opt-in now; C: run all kernel tests on every `verify` | A | | A, 2026-11-05 |
| Assertions from worker threads | A: not supported — test thread only, checked while the test is live; B: supported through an atomic counter with a lifetime contract | A | | A, 2026-11-05 |
| `UNITTESTS_ENABLED` and the unused `unit_tests_enabled` switchset field | A: leave the plumbing alone, linkage is the gate; B: define it for the test kernel and use it for test-only branches; C: delete the unused plumbing and `dbg.2`'s flag | A | | A, 2026-11-05 |
| Kernel-fault tests | A: no in-kernel recovery; a later increment is a host-orchestrated death test with one boot per expected death; B: catch the fault in the handler and resume the run; C: leave faults out entirely | A | | A as the later direction, 2026-11-05; v1 keeps death tests a non-goal |

## Testing and verification

- Host, runtime unchanged: hsien's existing hosted tests (`system/lib/hsien/test/runtime_test.cpp`, `parameterized_runtime_test.cpp`) still pass; no runtime source changes are expected.
- Host, kernel interface: `//kernel/lib/ktest:filter-test` (gtest) covers the matcher (`*`, `?`, `:`, `-`, empty positive half, a pattern matching nothing, a pattern matching everything), the formatting of a failed check into the message buffer, and the counting rules the runner reports. It is in `//:default`, so `boot_check.py`'s host-tests line runs it.
- Host, runner script: `scripts/kshell_run.py`'s `ktest` verdict is pure logic over captured lines; it gets unit tests beside the other `scripts/test_*.py` files (Python `unittest`), covering summary parsing, the all-count comparison, missing summary, `failed>0` and `ran=0`.
- Section and images: `llvm-nm`/`llvm-readelf` on the unstripped `vmours.with-tests` ELF shows `__hsien_tests_start`/`__hsien_tests_end` bounding the records in `.data.rel.ro`, and the plain `vmours` ELF shows no such section (or an empty range); both images still boot.
- Boot, interface end to end: after `python3 .agents/skills/verify/scripts/boot_check.py` builds both images, `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest list'` prints three `ktest: TEST` lines and `... 'ktest all'` prints one `RUN`/`OK` pair per test and `ktest: SUMMARY ran=3 passed=3 failed=0 skipped=0`; both exit 0.
- Failure path: during implementation, one pilot check is broken on purpose; `ktest all` must print a `FAIL` line with its `file:line`, the test's `FAILED` line and `failed=1`, and `kshell_run.py` must exit non-zero; then the check is restored and the same run passes. The temporary break is not committed.
- The three migrated tests must still check what they checked: the change's diff is the boilerplate removed and the wrappers added, the suite passes, and `python3 scripts/cov_kernel.py` reports their code as covered through the new `ktest list` phase.
- Threaded pilot: `SMP=1` for a run of `ktest Semaphore.*` and `ktest all`, since the semaphore test's slow path is the scheduler; the other CPUs are live in every run.
- Both images still boot exactly as before: `verify` passes with no change to its expectations; the rel image now links no test code and the dbg image links it, and neither change is visible in the boot log because nothing runs at boot.
- What cannot be verified yet: arm and riscv (no bootable image), real hardware in general, and a build with `kernel_shell_enabled=false` beyond compiling and linking (the tests are linked but no console can start them); per-test isolation and timeouts do not exist, so they cannot be verified.

## Open questions

- The `KTEST_DEATH`/`KTEST_EXPECT_PANIC` API shape, if the death-test increment is ever built: which faults (CPU exception vector, panic message, or both) a death test can expect, and whether the host's death-test loop belongs in `kshell_run.py` or a sibling script.
- Whether the kernel-mode exception path should reach `vpanic()` after its register dump in every image, rather than halting. That is what a host-orchestrated death test needs, and it is a kernel-wide change: the halt loses the crash record and the reboot that `kernel.panic.reboot` would give, so the question is worth its own decision, not a test-task side effect.
- The July annotation scanner could return later as a second declaration front end: `hsien::TestRecord` is a narrow enough seam that the macro and the scanner could both emit it. Is that wanted, and on which compiler? Not in v1.
- `UNITTESTS_ENABLED`: wire it for the test kernel, or delete the unused switchset plumbing and `dbg.2`'s flag with it. v1 ignores it.
- Whether `verify` should eventually run the whole kernel suite by default, and how long a fully migrated suite takes in QEMU.
- When a test moves out of `kernel/tests/` to live next to the subsystem it tests, and what the aggregate over subsystem `tests/` directories looks like. v1 keeps one directory and a manual list.
- Whether tests want a description string for `ktest list` (a third macro argument), which C++ cannot read as a docstring but can take as a literal.
- Per-test timeout and isolation: both need a seam the kernel does not have (a watchdog deadline; a runner thread or address space). Not designed yet.
- Whether a `ktest` run should make the online-CPU mask invariant check Zircon's runner makes. Our hotplug and preempt tests change the machine on purpose, so the check would have to be opt-in per test or per run.
