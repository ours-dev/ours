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

## Title: parameterized and typed tests — one declaration, one record per value or type (2026-11-07)

**Files**

- `kernel/lib/ktest/include/ktest/instantiate.hpp` — new: `ktest::internal::ParamSource` (the `GetParam()` holder), `InstantiationName` (the compile-time `Suite.Name/N` string), `RecordBlock` (the constexpr records array), `TypeList`, and the five macros `KTEST_VALUE_PARAM`, `KTEST_P`, `KTEST_F_P`, `KTEST_TYPE_LIST`, `KTEST_T`.
- `kernel/lib/ktest/include/ktest/fixture.hpp` — `run_fixture` takes trailing construction arguments (`Args &&...args` forwarded to `std::construct_at`), so a parameterized test's object can be built with its table value; `KTEST_F` passes none and its behavior is unchanged.
- `kernel/lib/ktest/include/ktest/ktest.hpp` — includes the new header, so one include still gives a test everything.
- `kernel/lib/ktest/test/host_shim.hpp`, `host_shim.cpp` — new: the runner stand-in (the six `ktest::internal` functions, the shared state, `run_record`) moved out of `fixture_test.cpp` so two host files can drive records.
- `kernel/lib/ktest/test/fixture_test.cpp` — loses its private stand-in, keeps every check.
- `kernel/lib/ktest/test/instantiate_test.cpp` — new: the host suite, 14 cases against the real macro expansions.
- `kernel/tests/ktest_param_test.cpp` — new: the booted smoke (a `KTEST_P` over 3 values, a `KTEST_F_P` over 2, a `KTEST_T` over 2 types).
- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/test/BUILD.gn`, `kernel/tests/BUILD.gn` — the new header, the shim + host file, the `ktest_param` target (and its place in the `kernel.tests` aggregate).
- `.clang-tidy` — `GetParam` joins `SetUp`/`TearDown` in the method-case exemption: GoogleTest's API vocabulary the interface re-exports.
- `kernel/lib/ktest/README.md`, `docs/design/261105/kernel-test-framework.md` — the feature documented where the fixture layer is.

**Symptom**

None — this is feature batch 3. v1 filed parameterized and typed tests under non-goals (`docs/design/261105/kernel-test-framework.md`), so the only way to run one body over many values or types was to hand-write one `KTEST` per instantiation and keep the tables in sync by hand.

**Cause**

GoogleTest's parameterized machinery (ValueParametrizedTestSuite, TYPED_TEST) builds its records at dynamic-init time through a per-test static initializer and a heap-allocated registry, which is the exact mechanism this framework's registration rule rules out — "a linker section, not a per-test static initializer" (`fixture.hpp`'s rationale, the design's Registration section). What was missing was a way to keep the GoogleTest contract (one body per declaration, one record per value or type, `/N` names) with records that are constant-initialized parts of the image: the count of records is known only to the compiler (it is the table's length), so a macro that cannot count could not simply write N records the way `KTEST` writes one.

**Change**

- The count problem is solved the way the records problem was: in the type system, not the preprocessor. One `KTEST_P(Suite, Name)` expands to the test class, and a constexpr records block built by expanding a `std::make_integer_sequence<usize, Suite_ParamBase::COUNT>` — the table's length, read at compile time through the class `KTEST_VALUE_PARAM(Suite, int, MyValues, 1, 42, 99)` declares. Each expansion instantiates a `Holder<Index>` whose `TestRecord` is `{name, &invoke<Index>, location}`, with the name built by a `consteval` constructor that appends `/` and the decimal index to the `Suite.Name` prefix — the string arithmetic the runner's own decimal writer does, hoisted into the image. The whole block lands in `.hsien.tests` as one constexpr object; the section walk stays a flat array of `TestRecord` because every element is `sizeof(TestRecord)` apart and the block's alignment is the record's, so no padding can land between the section's objects.
- The body reads the current value with `GetParam()` (the GoogleTest spelling, chosen over a `value_` member because it cannot collide with a fixture member in the composed macro). The class `KTEST_VALUE_PARAM` declares (`Suite_ParamBase`) derives from `ktest::internal::ParamSource<Type>` and carries the table's type, length and elements; `KTEST_P` derives its test class from `ktest::Fixture` and that base, `KTEST_F_P` from the author's fixture and that base. The param base deliberately does not derive from `Fixture`: it does, then `KTEST_F_P`'s object would carry two `Fixture` subobjects and `test->SetUp()` would be ambiguous. The adapter's only change is that `run_fixture` now forwards trailing construction arguments to `std::construct_at`, so the object is born with its table value and the whole `KTEST_F` lifecycle — fresh object per record, SetUp, body, TearDown — comes free.
- Typed tests are the same machinery over types instead of values: `KTEST_TYPE_LIST(Suite, (A, B, C))` declares a `TypeList`, and `KTEST_T(Suite, Name)`'s block builder pairs each type with its list position in one pack expansion (`Holder<Index, T>::RECORD...` over `TypeList<Types...>` and an integer sequence), so no index-of-type lookup is needed. The body is a static function template whose parameter is named `T`.
- The runner changed by zero lines: each instantiation is an ordinary `TestRecord`, so duplicate detection, filtering (globs match the `/N` suffix like any other character), sharding (the record's section index), shuffling, the suite plan (a `KTEST_P` suite's prefix is its records' suite) and the summary count treat instantiations exactly like plain tests.
- Two compile-time traps the first expansion hit, both worth remembering: an in-class `static constexpr` member's initializer cannot instantiate its own class's member templates (`BLOCK` is now called from the namespace-scope block variable, after the class is complete), and a macro parameter named `name` is substituted inside the field designator `.name` (`KTEST_T`'s parameters are spelled `suite_token`/`name_token`, like `KTEST_P_INSTANTIATE`'s).

**Reference**

GoogleTest 1.18.0, `TEST_P`/`INSTANTIATE_TEST_SUITE_P`/`TYPED_TEST_SUITE` (`vendor/googletest/googletest/include/gtest/internal/gtest-internal.h`, the `GTEST_TEST_` expansion and the parameterized registries built in `ParameterizedTestSuiteInfo::RegisterTests()`), and its `/N`-suffixed instantiation names (`src/gtest.cc`, `TestNameSuffix`). Deliberately different: the records are a constexpr array in the image instead of entries a dynamic-init pass appends to a heap registry, the table is a plain constexpr array the author names instead of a `::testing::Values` generator, the instantiation name is the prefix plus `/N` without GoogleTest's middle segment (the generator's name carries no information the filter cannot express), and `KTEST_VALUE_PARAM` binds the table to its suite at declaration so no separate instantiation statement exists to forget.

**Verification**

- Host: `ktest-test` 60 cases green (14 new); `scripts/cov_host.py` — `instantiate.hpp` 100 % lines (15/15; the two block-builder bodies are `consteval`, so they are compile-time only and no longer counted as unreachable runtime lines), `fixture.hpp` 96.5 % (137/142).
- Boot: `boot_check.py` green (dbg 46/5/0, rel 44/8/0, host-tests 183/0).
- `ktest list` names 77 tests (was 67): the 10 new instantiations listed by name — `KtestParam.ValueIsPositive/0` through `/2`, `KtestParam.ValueIsOneOfTheTables/0` through `/2`, `KtestParamFixture.SeesFixtureAndParameter/0..1`, `KtestTyped.SizeIsKnown/0..1`.
- `ktest KtestParam.*`: `ok, 6 ran, 6 passed, 0 skipped`. `ktest KtestParamFixture.*`: `ok, 2 ran, 2 passed`. `ktest KtestTyped.*`: `ok, 2 ran, 2 passed` (the first smoke draft failed record `/1` honestly — `alignof` of an empty class is 1, not its declared constant's — which is the coverage build proving the checks run per type).
- One instantiation: `ktest KtestParam.ValueIsPositive/1` → `ok, 1 ran, 1 passed`. Shard over the whole registry: `ktest all shard=76/77` → exactly one record. Shuffle: `ktest all shuffle=1 shard=1/77` → a `SHUFFLE` line whose order is registry indices and one green record.
- `ktest all`: `ok, 77 ran, 75 passed, 0 failed, 2 skipped` — `ran` equals the `ktest list` count, and the two skips are the pre-existing ones (`Ktest.SkipIsReported`, one tick test), so the instantiations added ten passing entries and no verdict-shape change.
- clang-tidy clean on every touched file (the pre-existing `hsien/test.hpp` naming noise is unchanged); `format_cpp.py`/`format_gn.py` clean; `gn check` reached through the builds above.

**Deliberately not done**

- `KTEST_F_T` (fixture + typed composition): the typed body is a free static function template, not a member of a fixture-derived class; composing the two would need a second base-and-ctor shape for little value this round. The fixture batch's `KTEST_F_P` covers the shared-state need.
- A middle name segment for instantiations (GoogleTest's `Suite/GeneratorName.Name/N`): the `/N` suffix alone is what filters need, and the generator name has no reader in this grammar.
- A per-instantiation value printer in failure lines: a failed check already prints the expression text and both values through the assertion layer; the record name carries `/N`.
- Empty tables and type lists: `RecordBlock`'s `static_assert` rejects them at compile time (a zero-length array would not link); no diagnostic text beyond the assert.
- `GetParam()` outside a test body (in a helper the body calls): the helper has no `this` of the test class, which is the same caveat GoogleTest documents; nothing reports it specially.

**How it was found**

None.

**The user's prediction**

None.

**Three questions you should be able to answer**

1. The macro cannot count the table's values, so where does the record count actually come from at compile time, and what one expression turns a table's length into `Suite.Name/0..N-1` records?
2. Why must `Suite_ParamBase` not derive from `ktest::Fixture` for `KTEST_F_P` to work, and what would a body see if the two fixtures each carried their own `Fixture` subobject?
3. The records block is one object in `.hsien.tests`, not N separate records — what two facts about its layout keep the runner's flat section walk correct?
