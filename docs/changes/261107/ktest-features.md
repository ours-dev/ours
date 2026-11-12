# ktest-features changes


## EXPECT_THAT with the core matcher set (2026-11-12)

**Files**
- `kernel/lib/ktest/include/ktest/matchers.hpp` (new) — the matchers and the `EXPECT_THAT`/`ASSERT_THAT` macros.
- `kernel/lib/ktest/include/ktest/ktest.hpp`, `kernel/lib/ktest/test/{matcher_test.cpp,ktest_test.cpp}`, `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/test/BUILD.gn`, `.clang-tidy`, `kernel/tests/ktest_test.cpp`, `kernel/tests/ktest_param_test.cpp` — wiring, smoke tests, and the tidy exemptions.

**Symptom**
Latent: assertions spelled the expectation as bare comparisons; compound conditions read as C, not as intent.

**Cause**
The design excluded matchers from the first slice; the user asked for the excluded set on 2026-11-07.

**Change**
`EXPECT_THAT(value, matcher[, message])` / `ASSERT_THAT(...)` with: `Eq/Ne/Lt/Le/Gt/Ge(x)` (any comparable T), `IsNull()/NotNull()`, `Ok()` (Status via to_string), and `AllOf`/`AnyOf` of two to four matchers. Matchers are constexpr structs carrying their expected value by value — no heap; the failure record names the value expression, what the matcher wanted and what it saw, through the same KTEST_RECORD_MESSAGE grammar the EXPECT family uses.

**Reference**
GoogleTest's `EXPECT_THAT` + `testing::*` matchers (`include/gtesting/gmock`); the freestanding subset: no monomorphic/polymorphic matcher base classes with heap-held descriptions — descriptions are fixed-format strings.

**Verification**
Host: matcher_test 71 cases green; cov_host matchers.hpp 98.7% lines; booted `Ktest.MatcherSmoke` ran=1 passed=1; boot_check dbg 46/5/0, rel 44/8/0 7/7 required, host-tests 183/0.

**Deliberately not done**
`Throws` (no exceptions), container matchers (no iterators over kernel collections), string matchers beyond Eq (no heap), printing arbitrary user types (formatter registration is a separate feature).

**How it was found**
None (feature work; the originating agent hit its quota and the session finished the remainder).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a matcher a constexpr struct and not a lambda?
- What does `Ok()` print on failure, given Status has no formatter?
- Why do AllOf/AnyOf cap at four children?
