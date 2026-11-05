# kernel-test-framework: review of the kernel test interface change (484e5273..e91ba243)

**Date**: 2026-11-05

## Scope and method

The five commits that build the kernel test interface and its host side: `484e5273` (the library and the section move), `3ed64780` (the first records and the image swap), `df23558b` (the host verdict), `9534e49b` (the pilot migration and coverage discovery), plus the design and plan documents (`b208036d`, `04c41930`, `10fa4ae1`, `95cb84c9`, `e91ba243`). 42 files, +2293/-404.

Authorities read before the review: `modern-cpp-development`, `kernel-design` (and its `layering.md`/`naming.md` references), `.clang-format`, `.clang-tidy`, and the surrounding files (`system/lib/hsien` for the runtime, `kernel/commands` for the style the migrated tests came from, `scripts/cov_kernel.py` and `scripts/host_tests.py` for the host side). The full files were read, not only the diff.

Evidence run on the tree:

- `python3 scripts/format_cpp.py --check` on every changed C++ file: clean.
- `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86` on `kernel/lib/ktest/src/runner.cpp`, the three host test translation units, and the three migrated tests: no diagnostic in the changed files (the only output is pre-existing `system/lib/ustl` header noise).
- `python3 scripts/test_kshell_run.py` (17 cases) and `python3 scripts/test_cov_kernel.py` (19 cases): all pass.
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok; dbg PASS 46/5/0, rel PASS 44/8/0 with 7/7 required; host-tests 173/0.
- `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest all'`: `SUMMARY ran=10 passed=9 failed=0 skipped=1`, exit 0; the three migrated suites run on their own as well.
- `llvm-nm`/`llvm-readelf` on the dbg kernel: `__hsien_tests_start`/`__hsien_tests_end` bound three records in `.data.rel.ro` before the migration (the non-empty case is the coverage run's ELF).
- `python3 scripts/cov_kernel.py --out out/cov-kernel`: exit 0; 10 interface tests and 43 `*.test` commands; 74.4% lines (42,153/56,631).

Findings H1, H2 and M2 are read-only judgement from the code; M1 and m1 are confirmed by the commands above. No repducer was written for H1 or H2 (both need a misbehaving test, which the framework is supposed to diagnose rather than exhibit); the reasoning is in the items.

## What is done well

- The layering holds: `system/lib/hsien` keeps the record type, the context and the failure reporting; `kernel/lib/ktest` adds the declaration, assertions, filter and command, and nothing in the production kernels links it (`kernel/lib/ktest/BUILD.gn:31-45`, `kernel/tests/BUILD.gn:52-62`).
- The section move has the right reasoning in the right place: `kernel/kernel.ld:173-182` says why the records cannot live in the reclaimable `.init` tail, and the moved block is the only runtime-side edit.
- The host verdict is pure logic over captured lines and is unit-tested without QEMU (`scripts/kshell_run.py:409-532`, `scripts/test_kshell_run.py`), which is what keeps the migration's verification repeatable.
- The coverage consumer learns the registry from the booted kernel instead of a second source scrape (`scripts/cov_kernel.py:236-260`), and the list parser is a pure function with its own tests.
- The migrated tests kept every check, and the ones that read best are the semaphore tests: the worker threads publish counters and the test thread asserts (`kernel/tests/semaphore_test.cpp:230-296`), which is the interface's rule expressed in the test's own structure.

## High severity

### H1. The published run state and owner thread are plain globals, so a wrong-thread assertion is a data race

`kernel/lib/ktest/src/runner.cpp:26-31` declares `CURRENT_STATE` and `OWNER_THREAD` as plain pointers; the runner writes them at `:79-85`, and `record_failure` reads `CURRENT_STATE` from the assertion macro's thread (`kernel/lib/ktest/include/ktest/assertions.hpp:29-46`). The interface's documented rule is that assertions run only on the test thread, but the framework exists to *diagnose* a test that breaks the rule: a worker thread calling `EXPECT_*` reads those pointers while the runner stores them, which is a C++ data race — undefined behavior in the code that is supposed to report misuse cleanly. The design says the publication is "a single file-static `std::atomic<Context *>`" (`docs/design/261105/kernel-test-framework.md`, "Objects"), so the implementation drifted from its own design.

Worst interleaving: the runner finishes a test body and stores `CURRENT_STATE = nullptr`; a worker the test forgot to join calls `EXPECT_TRUE`, loads the pointer (possibly torn on a weaker architecture), and may either read a stale `RunState` whose stack frame is gone or dereference a pointer that the runner has already cleared — a use-after-free under a diagnostic path. Fix: make both `std::atomic` (relaxed load/store is enough for race-freedom; the lifetime contract stays the test's responsibility) and check the owner *before* dereferencing the state. The lifetime limit itself cannot be removed without reference counting the state, which the design deliberately avoids.

### H2. `KTEST_SKIP` keeps the caller's pointer and prints it after the test has returned

`record_skip` stores the argument in `RunState::skip_reason` (`kernel/lib/ktest/src/runner.cpp:192-205`, `kernel/lib/ktest/include/ktest/run_state.hpp:24`) and the runner prints it only after `hsien::run` returns (`runner.cpp:86-96`). The comment asks callers to pass a literal (`assertions.hpp:379-385`), and every current caller does, but a macro that accepts `char const *` cannot enforce it: `KTEST_SKIP(some_stack_buffer)` leaves a dangling pointer that the runner reads after the test's frame is gone. Fix: copy the reason into a fixed buffer in `RunState`, truncating; the framework's whole point is that a test's last words survive the test.

## Medium severity

### M1. `EXPECT_EQ` on two C strings compares addresses

`#define EXPECT_EQ(...) KTEST_COMPARE(==, ...)` (`kernel/lib/ktest/include/ktest/assertions.hpp:246`) compares the results of `==` on the declared types, so `EXPECT_EQ("a", "b")` compares two `char const *` and may pass or fail for the linker's reasons, not the content's. GoogleTest special-cases `char*`/`char[N]` in `FormatForComparison`; this interface deliberately does not carry that machinery yet, but the difference is a trap for test authors coming from that API. The minimum is a line in the header and in the README: compare `std::string_view` (or add `EXPECT_STREQ` when a test needs it). A real fix is a later matcher/printer increment.

### M2. A `ktest` command that ends at an `ktest: ERROR` line makes the host wait out the timeout

`scripts/kshell_run.py:144` and `:766` end a run only at `ktest: SUMMARY`. Two registry-level paths return before a summary: an empty registry ("no tests are linked into this kernel", `kernel/lib/ktest/src/runner.cpp:157-160`) and a duplicate name (`:161-164`); the usage error (`argc > 2`) does the same. On those the command has already printed an error line and is done, but the host keeps waiting for a summary until `--command-timeout` (120 s), with the silence monitor enabled (it is a `ktest` command), so the machine is NMI'd and the lockup report — a wrong diagnosis for a registry error — is printed before the timeout. Fix: end the wait on an `ktest: ERROR` line as well, and say in the verdict that the run ended at an error before its summary.

### M3. The summary's own arithmetic is not checked

`judge_ktest_run` (`scripts/kshell_run.py:491-532`) reads `ran`, `passed`, `failed` and `skipped` independently. A runner bug that printed `ran=3 passed=3 skipped=1` would pass the host untouched. The check is one subtraction and costs nothing: `passed + failed + skipped == ran` must hold.

## Minor and style

- `kernel/lib/ktest/README.md:64` gives the host test binary as `host/obj/kernel/lib/ktest/ktest-test`; the real path has the `test/` directory: `host/obj/kernel/lib/ktest/test/ktest-test`. Copying the line as written fails.
- `kernel/lib/ktest/include/ktest/format.hpp:35` computes `storage + room - 1`; `room == 0` is pointer arithmetic one before the array, which is undefined even if never dereferenced. Every caller passes a real buffer, so this is a precondition, not a bug; state it in the comment or take the end pointer instead.
- `products/bringup.gni` still carries `unit_tests_enabled = false` although `kernel.dev.dbg_1` now packages the test-capable kernel. The field is informational and the design's decision was "linkage is the gate" (decision 10), but the two now read inconsistently in `product_info.json`; either wire it or delete the unused field, as the design's open question says.
- The two migrated checks `EXPECT_EQ(status, Status::Ok)` in `kernel/tests/semaphore_test.cpp` print `<unprintable>` for both values when they fail; `EXPECT_OK(status)` says "expected Status::Ok, actual <name>" through `to_string`, which is the message the interface added `EXPECT_OK` for. Naming the right macro is also the teaching example.

## Teaching opportunities

### T1. Publication needs an atomic; lifetime needs a contract

The invariant behind H1: the runner owns the `RunState`, publishes it before the test body and withdraws it after; the assertions are its only readers. In the *intended* usage there is one thread, so no synchronization is needed beyond ordinary program order. The moment a worker thread reads at all — the misuse the owner check is there to catch — the runner's stores and the worker's loads are concurrent. `std::atomic<RunState *>` with relaxed accesses makes the race go away at the pointer level without imposing ordering, because the check only asks "is this null, and is it the same thread", neither of which publishes data to another thread. What atomics cannot fix is that a worker that reads the pointer before withdrawal and dereferences it after the runner's stack frame ended still has a use-after-free; that is why the contract is "assertions on the test thread only" and why the design refuses reference counting for a test-only state. The worst interleaving is in H1.

### T2. A macro parameter is a token name, and the rest of the macro does not know that

`KTEST(Ktest, AssertionsPass)` first expanded to `.AssertionsPass = "Ktest" "." "AssertionsPass"`, because the designator `.name` contained the macro parameter `name`. Then the anonymous namespace's closing brace sat between the body declaration and the author's `{`, so the body was defined as `void ktest_body_17() } {`. Both were fixed by renaming the parameters (`suite_token`, `name_token`, `line_number`) and moving the brace; the lesson is to name macro parameters for what they are and to write the expansion out on paper (or in `-E`) before trusting it (`kernel/lib/ktest/include/ktest/ktest.hpp:33-53`).

### T3. A function in a header is a definition in every translation unit that includes it

`ktest::internal::finish()` was not `inline`; the host test had one translation unit including `format.hpp`, the three kernel test files gave it three, and the kernel link failed with `multiple definition`. The rule is not "headers cannot define functions" but "a non-template function defined in a header must be `inline`" (constexpr implies it; `finish` is not constexpr because it writes through a pointer). The reason the host test missed it is a good reminder that a passing single-TU test does not prove header hygiene (`kernel/lib/ktest/include/ktest/format.hpp:100`).

### T4. Assertion macros capture types, and `-Wsign-compare` sees them

The command form `verdict(sema.count() == 0, ...)` compiled; the macro form `EXPECT_EQ(sema.count(), 0)` did not, because `KTEST_COMPARE` binds both operands as `auto const &` and compares them directly, so `u64` against `int` is a signed/unsigned comparison under `-Werror`. The fix is to type the literal (`u64{0}`), not to cast the count. This is the same reason a comparison macro is harder to design than a function: it gives up the implicit conversions a hand-written expression would have used (`kernel/tests/semaphore_test.cpp:144,158,161,201,203`).

## Suggested follow-up order

1. H1 — atomic publication and owner check before dereference (`kernel/lib/ktest/src/runner.cpp`, `assertions.hpp`).
2. H2 — copy the skip reason (`runner.cpp`, `run_state.hpp`, the comment in `assertions.hpp`).
3. M2 — end the host's wait on `ktest: ERROR`, and M3 — check the summary's arithmetic (`scripts/kshell_run.py`, `scripts/cov_kernel.py`, its unit tests).
4. M1 — document the C-string comparison difference in `assertions.hpp` and the README.
5. m1 — fix the README path.
6. m2, m3 — a room check in `make_buffer`, and the `unit_tests_enabled` metadata: both want a decision rather than a quick edit.
