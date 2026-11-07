# kernel-test-framework changes

## Title: fixtures for ktest — SetUp/TearDown per test, a suite hooked once, and a global environment (2026-11-07)

**Files**

- `kernel/lib/ktest/include/ktest/fixture.hpp` — new: `ktest::Fixture`, `ktest::internal::SuiteHook` and `SuitePlan` (the once-per-suite decisions), `ktest::internal::run_fixture` (the lifecycle the adapter runs), and the `KTEST_F`/`KTEST_SUITE_SETUP`/`KTEST_SUITE_TEARDOWN` macros.
- `kernel/lib/ktest/include/ktest/environment.hpp` — new: `ktest::Environment`, the fixed-capacity registry, `ktest::add_environment`, and `internal::EnvironmentRun`, the setup-then-reverse-teardown walk.
- `kernel/lib/ktest/src/environment.cpp` — new: the one registry definition and the walk's two loops.
- `kernel/lib/ktest/src/runner.cpp` — the `.ktest.suite_hooks` section reader, hook framing (`run_hook`), the environment passes, and the suite plan threaded through the run loop; `run_test` grew a `pre_failed` verdict for a test a failed setup keeps the body from.
- `kernel/lib/ktest/include/ktest/ktest.hpp` — includes the two new headers, so one include still gives a test everything.
- `kernel/kernel.ld` — `.ktest.suite_hooks` beside `.hsien.tests` in `.data.rel.ro`, `KEEP`'d and bounded by `__ktest_suite_hooks_start`/`__ktest_suite_hooks_end`.
- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/test/BUILD.gn`, `kernel/tests/BUILD.gn` — the new headers, the `environment` source set (host and kernel both link it), the host `fixture_test.cpp`, and the booted `ktest_fixture` target.
- `kernel/lib/ktest/test/fixture_test.cpp` — new: the host suite, 24 cases against the real macro expansions.
- `kernel/tests/ktest_fixture_test.cpp` — new: the booted smoke, all passing by default.
- `.clang-tidy` — `SetUp`/`TearDown` exempted from the method-case rule.
- `docs/design/261105/kernel-test-framework.md`, `kernel/lib/ktest/README.md` — the fixture layer documented where run options were.

**Symptom**

None — this is feature batch 2. v1 filed fixtures, suite setup and a global environment under non-goals (`docs/design/261105/kernel-test-framework.md`), and a test that needed shared state could only stuff it in file-scope statics or rebuild it inside every body.

**Cause**

The v1 design deliberately deferred the fixture family, and for one reason that mattered to the shape it deferred: GoogleTest builds its per-test fixture object on the heap, inside a per-test static initializer, and this kernel's registration rule is the opposite — "a linker section, not a per-test static initializer" — because the registry must be part of the image, need no boot-time constructor and no allocation. What was missing was a way to keep GoogleTest's *contract* (SetUp before the body, TearDown after it however the body ended, a suite hooked once, a global stage) while dropping GoogleTest's *mechanism*.

**Change**

- The per-test lifecycle lives in one template the macro's adapter calls, and it does not need the runner to change a single verdict rule: `ktest::internal::run_fixture<Test>(context, storage, body)` constructs the test's object in the macro's own static storage (`alignas(Test) static unsigned char ktest_fixture_storage_...[sizeof(Test)]`, no heap, no dynamic initialization), runs `SetUp`, runs the body only when SetUp recorded nothing (`context.failure_count()` unchanged and the run state not skipped), then runs `TearDown` and destroys the object. Every failure — SetUp, body, TearDown — lands in the same `hsien::Context`, so the runner's existing `recorded.failed != 0` verdict covers the whole lifecycle and a fixture test reports exactly like a plain one.
- `KTEST_F(Fixture, Name)` follows GoogleTest's expansion shape: a file-scope `Fixture_Name_Test final : public Fixture` whose `ktest_body()` the user's braces define, plus the record (`Fixture.Name` in `.hsien.tests`) and the storage, both in an anonymous namespace. Unlike `KTEST`, the macro's tokens are name-based, not line-based, so a host test can name the record and drive it — that is what makes the host suite test the real expansions.
- Suite hooks are a second kernel-owned registry, `.ktest.suite_hooks`, holding `ktest::internal::SuiteHook {name, suite, kind, invoke}` — the same second-section pattern the design already sketched for death tests, and the same reuse rule as `.hsien.tests`: records found by walking the range, `KEEP`'d, read on demand. The runner builds one `SuitePlan` per pass over the hooks, the records and the run's own selection (filter and shard), and asks it `before`/`poisoned`/`after` around each test; once-per-suite and teardown-after-last survive suites whose records are disjoint in section order, because remaining is a count, not an adjacency test.
- Environments register through `ktest::add_environment` into a fixed table (8 slots, overflow counted and reported at run time, never silently dropped). `EnvironmentRun` runs the SetUps in registration order, stopping at the first failure, and unwinds the attempted prefix in reverse — including the environment whose SetUp failed, which is what makes TearDown the place to undo a half-finished stage.
- Failure semantics this batch decided where the request was silent: a suite setup failure poisons its suite (each test prints `RUN` + `FAILED (its suite setup failed)` without running body or fixture hooks; the suite teardown still unwinds), an environment setup failure poisons the run the same way, and `KTEST_SKIP` in a SetUp leaves the body out exactly like a failed check. With the concurrent run-plan batch, suite hooks frame each pass (repeat hands every suite a fresh stage), while the environment spans the whole command.
- On the host, the sections do not exist (`[[gnu::section(".hsien.tests")]]` does not even compile on Mach-O), so `KTEST_RECORD_SECTION` drops the attribute under `#if !defined(__ELF__)` and the host tests name the records directly; the host suite also stands in for the six `ktest::internal` functions `runner.cpp` defines. That is the seam split: everything about the macros except where the records land is host-testable.

**Reference**

GoogleTest 1.18.0, `TEST_F` and `Test::SetUp`/`Test::TearDown` (`vendor/googletest/googletest/include/gtest/internal/gtest-internal.h:1484-1521`, the expansion the design already cited), `testing::Environment`/`AddGlobalTestEnvironment` (`include/gtest/gtest.h`), and `SetUpTestSuite`/`TearDownTestSuite` for the suite hooks. Deliberately different: the per-test object is placement-constructed in the macro's static storage (`std::construct_at`/`std::destroy_at`) instead of heap-allocated by a registry; the suite hook is a macro-emitted record in its own section instead of a static member the framework discovers; suite and environment failures are decided by this design (GoogleTest leaves SetUpTestSuite failure semantics undefined at its API level).

**Verification**

- Host: `ktest-test` 46 cases green (24 new, `scripts/cov_host.py`: 183/183 across the build); coverage `fixture.hpp` 96.5 % lines (136/141), `environment.cpp` 100 % (16/16), `environment.hpp` pure declarations after the walk moved into the .cpp. The coverage build caught a real bug before the gate did: two environment cases registered stack objects without restoring the registry, and a later walk called into destroyed objects — UB that the normal build survived silently.
- Boot: `boot_check.py` green (dbg 46/5/0, rel 44/8/0, host-tests 183/0).
- `ktest KtestFixture.*`: `ok, 3 ran, 3 passed, 0 skipped`. `ktest all`: `ok, 67 ran, 65 passed, 2 skipped` — `ran` still equals the `ktest list` count, now 67.
- Failure path by hand: `ktest KtestFixture.* fail=1` fails the first test; the log shows its TearDown still ran (the third test's teardown counter check passed), the suite setup still counted once, the suite teardown still came after, and the run ended `SUMMARY ran=3 passed=2 failed=1`.
- `gn check` (whole build) OK; clang-tidy clean on every touched file; `format_cpp.py`/`format_gn.py` clean.

**Deliberately not done**

- `RecordProperty`, parameterized/typed tests, matchers, death tests — the design's non-goals, unchanged.
- No `EXPECT`/`ASSERT` distinction inside SetUp: the runner cannot tell them apart (both just count a failure), so any recorded failure skips the body. In-kernel, a failed precondition is not one to continue past.
- Suite hooks and environments print nothing on success — no `RUN`/`OK` lines — so `kshell_run.py`'s `ran == listed` check stays exact and the grammar gains no new line type. A failure is visible through the `FAIL` line (named `Suite.SuiteSetUp`) plus one `ERROR` line.
- No deduplication of `add_environment` for the same object (GoogleTest also runs duplicates twice); capacity 8 with a counted drop is the whole contract.

**How it was found**

None.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. Why does a fixture test need no change at all in the runner's verdict logic, and where does the "SetUp failed, skip the body" decision actually live?
2. Where does the fixture object live, and what two things would have broken if the macro had built it on the heap inside a static initializer instead?
3. If a suite's records are split across translation units — `Foo.A`, `Bar.X`, `Foo.B` in section order — what makes the suite teardown still run exactly once, after `Foo.B`?
