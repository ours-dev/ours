/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The matcher half of the assertion family, on the host: each matcher's
// verdict and the description its failure carries, the combinators over
// their fixed arity, the ASSERT_THAT early return, and the Status that
// renders through to_string — all against the real macro expansions driven
// through hsien's own record loop.
//
// The bodies hang on a no-state fixture: EXPECT_THAT is just an assertion,
// so the fixture carries nothing — it exists because a KTEST_F's record has
// a name-addressable symbol, which is what lets the cases drive each body
// by name the way the booted runner drives a record.
//
// This file includes ktest.hpp first, so the bodies write the
// KTEST_-prefixed spellings — the machinery under test — while the bare
// names stay gtest's for the TEST cases, whose EXPECT_THAT is gtest's own
// matcher macro. gtest.h redefining the bare names on arrival is therefore
// expected and silenced here alone.
#include <ktest/ktest.hpp>

#include "host_shim.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <gtest/gtest.h>
#pragma clang diagnostic pop

#include <string>

#include <hsien/runner.hpp>
#include <ours/status.hpp>

namespace {

/// How many times the value-and-matcher-once case's expressions ran.
usize ARGUMENT_EVALS = 0;

auto counted() -> int {
    ARGUMENT_EVALS += 1;
    return 42;
}

/// The description one matcher renders, as a failure line would carry it.
template <typename Matcher>
auto described(Matcher const &matcher) -> std::string {
    char storage[ktest::internal::MESSAGE_ROOM];
    auto buffer = ktest::internal::make_buffer(storage, sizeof(storage));
    matcher.describe(buffer);
    return std::string{ktest::internal::finish(buffer)};
}

// --- the bodies the verdict cases drive ---

/// EXPECT_THAT is just an assertion: the fixture carries no state, and the
/// bodies prove the forms compile and record what they should beside a
/// KTEST_F's ordinary lifecycle.
class MatcherBodies: public ktest::Fixture {};

KTEST_F(MatcherBodies, ComparePasses) {
    KTEST_EXPECT_THAT(42, ktest::Eq(42));
    KTEST_EXPECT_THAT(41, ktest::Ne(42));
    KTEST_EXPECT_THAT(41, ktest::Lt(42));
    KTEST_EXPECT_THAT(42, ktest::Le(42));
    KTEST_EXPECT_THAT(43, ktest::Gt(42));
    KTEST_EXPECT_THAT(42, ktest::Ge(42));
    // Any actual comparable to the expected value: a wider integer beside a
    // narrow literal, a double beside an int.
    KTEST_EXPECT_THAT(i64{42}, ktest::Eq(42));
    KTEST_EXPECT_THAT(42.5, ktest::Gt(42));
}

KTEST_F(MatcherBodies, CompareFails) {
    auto const value = 41;
    KTEST_EXPECT_THAT(value, ktest::Eq(42));
}

KTEST_F(MatcherBodies, CompareFailsWithMessage) {
    auto const value = 41;
    KTEST_EXPECT_THAT(value, ktest::Eq(42), "after {} reads", 3);
}

KTEST_F(MatcherBodies, PointerPasses) {
    auto value = 0;
    auto *const live = &value;
    KTEST_EXPECT_THAT(live, ktest::NotNull());
    int *const dead = nullptr;
    KTEST_EXPECT_THAT(dead, ktest::IsNull());
    KTEST_EXPECT_THAT(nullptr, ktest::IsNull());
}

KTEST_F(MatcherBodies, PointerFailsOnNonNull) {
    auto value = 0;
    KTEST_EXPECT_THAT(&value, ktest::IsNull());
}

KTEST_F(MatcherBodies, PointerFailsOnNull) {
    int *const dead = nullptr;
    KTEST_EXPECT_THAT(dead, ktest::NotNull());
}

KTEST_F(MatcherBodies, StatusPasses) {
    KTEST_EXPECT_THAT(ours::Status::Ok, ktest::Ok());
    KTEST_EXPECT_THAT(ours::Status::InvArgs, ktest::Eq(ours::Status::InvArgs));
}

KTEST_F(MatcherBodies, StatusFails) {
    auto const status = ours::Status::InvArgs;
    KTEST_EXPECT_THAT(status, ktest::Ok());
}

KTEST_F(MatcherBodies, StatusFailsAgainstNamed) {
    auto const status = ours::Status::Ok;
    KTEST_EXPECT_THAT(status, ktest::Eq(ours::Status::InvArgs));
}

KTEST_F(MatcherBodies, AllOfPasses) {
    KTEST_EXPECT_THAT(42, ktest::AllOf(ktest::Ge(40), ktest::Le(45)));
    KTEST_EXPECT_THAT(42, ktest::AllOf(ktest::Ge(40), ktest::Le(45), ktest::Eq(42)));
    KTEST_EXPECT_THAT(42, ktest::AllOf(ktest::Ge(40), ktest::Le(45), ktest::Eq(42), ktest::Ne(41)));
}

KTEST_F(MatcherBodies, AllOfFails) {
    auto const value = 50;
    KTEST_EXPECT_THAT(value, ktest::AllOf(ktest::Ge(40), ktest::Le(45)));
}

KTEST_F(MatcherBodies, AnyOfPasses) {
    KTEST_EXPECT_THAT(42, ktest::AnyOf(ktest::Eq(41), ktest::Eq(42)));
    KTEST_EXPECT_THAT(42, ktest::AnyOf(ktest::Eq(41), ktest::Eq(43), ktest::Eq(42)));
    KTEST_EXPECT_THAT(42, ktest::AnyOf(ktest::Eq(41), ktest::Eq(43), ktest::Eq(44), ktest::Eq(42)));
}

KTEST_F(MatcherBodies, AnyOfFails) {
    auto const value = 42;
    KTEST_EXPECT_THAT(value, ktest::AnyOf(ktest::Eq(41), ktest::Eq(43)));
}

KTEST_F(MatcherBodies, AssertReturns) {
    KTEST_ASSERT_THAT(41, ktest::Eq(42));
    KTEST_RECORD_MESSAGE("the assert did not return");
}

KTEST_F(MatcherBodies, AssertPasses) {
    KTEST_ASSERT_THAT(42, ktest::Eq(42));
}

KTEST_F(MatcherBodies, EachArgumentOnce) {
    ARGUMENT_EVALS = 0;
    KTEST_EXPECT_THAT(counted(), ktest::Eq(42));
    KTEST_EXPECT_THAT(41, ktest::AnyOf(ktest::Eq(counted()), ktest::Eq(41)));
    KTEST_EXPECT_TRUE(ARGUMENT_EVALS == 2, "the value and the matcher were each evaluated once");
}

// --- the cases ---

TEST(KtestMatcherConcept, VerdictsAreCompileTimeAnswers) {
    // A matcher is an object carrying its expected value, not a callback
    // into a framework: the verdicts answer at compile time.
    static_assert(ktest::Eq(42).matches(42), "Eq holds when it should");
    static_assert(!ktest::Eq(42).matches(41), "Eq fails when it should");
    static_assert(ktest::Ne(42).matches(41), "Ne holds when it should");
    static_assert(ktest::Lt(42).matches(41), "Lt holds when it should");
    static_assert(ktest::Le(42).matches(42), "Le holds when it should");
    static_assert(ktest::Gt(42).matches(43), "Gt holds when it should");
    static_assert(ktest::Ge(42).matches(42), "Ge holds when it should");
    static_assert(ktest::AllOf(ktest::Ge(1), ktest::Le(9)).matches(5),
                  "AllOf holds when every child does");
    static_assert(!ktest::AllOf(ktest::Ge(1), ktest::Le(9)).matches(50),
                  "AllOf fails when a child does");
    static_assert(ktest::AnyOf(ktest::Eq(1), ktest::Eq(9)).matches(9),
                  "AnyOf holds when a child does");
    static_assert(!ktest::AnyOf(ktest::Eq(1), ktest::Eq(9)).matches(5),
                  "AnyOf fails when no child does");
    static_assert(ktest::Ok().matches(ours::Status::Ok), "Ok holds on Status::Ok");
    static_assert(!ktest::Ok().matches(ours::Status::InvArgs), "Ok fails otherwise");
}

TEST(KtestMatcherConcept, PointerVerdicts) {
    auto value = 0;
    EXPECT_TRUE(ktest::NotNull().matches(&value));
    EXPECT_FALSE(ktest::IsNull().matches(&value));
    int *const dead = nullptr;
    EXPECT_TRUE(ktest::IsNull().matches(dead));
    EXPECT_FALSE(ktest::NotNull().matches(dead));
    EXPECT_TRUE(ktest::IsNull().matches(nullptr));
}

TEST(KtestMatcherConcept, DescriptionsNameTheExpectation) {
    EXPECT_EQ(described(ktest::Eq(42)), "equal to 42");
    EXPECT_EQ(described(ktest::Ne(42)), "not equal to 42");
    EXPECT_EQ(described(ktest::Lt(42)), "less than 42");
    EXPECT_EQ(described(ktest::Le(42)), "less than or equal to 42");
    EXPECT_EQ(described(ktest::Gt(42)), "greater than 42");
    EXPECT_EQ(described(ktest::Ge(42)), "greater than or equal to 42");
    EXPECT_EQ(described(ktest::IsNull()), "a null pointer");
    EXPECT_EQ(described(ktest::NotNull()), "a non-null pointer");
    EXPECT_EQ(described(ktest::Ok()), "Status::Ok");
    EXPECT_EQ(described(ktest::AllOf(ktest::Ge(1), ktest::Le(9))),
              "all of: greater than or equal to 1, less than or equal to 9");
    EXPECT_EQ(described(ktest::AnyOf(ktest::Eq(1), ktest::Eq(9))),
              "any of: equal to 1, equal to 9");
}

TEST(KtestMatcherConcept, AStatusRendersThroughToString) {
    // The type has no std::formatter; the matcher message names it the way
    // an EXPECT_OK failure does.
    EXPECT_EQ(described(ktest::Eq(ours::Status::InvArgs)), "equal to InvArgs");
}

TEST(KtestMatcherVerdict, EachPassingFormCountsItsChecks) {
    FAILURES.clear();
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_ComparePasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_PointerPasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_StatusPasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_AllOfPasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_AnyOfPasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_AssertPasses).failures, usize{0});
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_EachArgumentOnce).failures, usize{0});
    EXPECT_EQ(FRAMEWORK_ERRORS, usize{0});
}

TEST(KtestMatcherVerdict, FailureNamesExpectationAndActual) {
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_MatcherBodies_CompareFails);
    EXPECT_EQ(outcome.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    // The value expression as written, then what the matcher expected and
    // the value it saw — EXPECT_EQ's grammar, with the matcher's phrase.
    EXPECT_NE(FAILURES[0].find("value: expected equal to 42, actual 41"), std::string::npos);
}

TEST(KtestMatcherVerdict, FailureCarriesTheAuthorsMessage) {
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_MatcherBodies_CompareFailsWithMessage);
    EXPECT_EQ(outcome.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("actual 41 -- after 3 reads"), std::string::npos);
}

TEST(KtestMatcherVerdict, PointerFailuresNameThePointerExpectation) {
    FAILURES.clear();
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_PointerFailsOnNonNull).failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("expected a null pointer, actual"), std::string::npos);
    FAILURES.clear();
    EXPECT_EQ(run_record(ktest_record_MatcherBodies_PointerFailsOnNull).failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("expected a non-null pointer, actual"), std::string::npos);
}

TEST(KtestMatcherVerdict, StatusFailureNamesTheStatusThroughToString) {
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_MatcherBodies_StatusFails);
    EXPECT_EQ(outcome.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    // The actual status renders as its to_string, not <unprintable>.
    EXPECT_NE(FAILURES[0].find("status: expected Status::Ok, actual InvArgs"), std::string::npos);
    FAILURES.clear();
    auto const named = run_record(ktest_record_MatcherBodies_StatusFailsAgainstNamed);
    EXPECT_EQ(named.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("status: expected equal to InvArgs, actual Ok"), std::string::npos);
}

TEST(KtestMatcherVerdict, CombinatorFailureShowsTheWholeExpectation) {
    FAILURES.clear();
    auto const all = run_record(ktest_record_MatcherBodies_AllOfFails);
    EXPECT_EQ(all.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("value: expected all of: greater than or equal to 40, less than "
                               "or equal to 45, actual 50"),
              std::string::npos);
    FAILURES.clear();
    auto const any = run_record(ktest_record_MatcherBodies_AnyOfFails);
    EXPECT_EQ(any.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("value: expected any of: equal to 41, equal to 43, actual 42"),
              std::string::npos);
}

TEST(KtestMatcherVerdict, AssertReturnsFromTheBody) {
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_MatcherBodies_AssertReturns);
    // One failure, and the message after the assert never ran.
    EXPECT_EQ(outcome.failures, usize{1});
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_TRUE(FAILURES[0].find("did not return") == std::string::npos);
}

} // namespace
