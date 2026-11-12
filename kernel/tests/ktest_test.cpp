/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The interface's own smoke tests. They are here so the image that carries the
// test interface always carries at least one real record, and so a booted
// `ktest` run exercises the declaration macro, both assertion families and the
// skip state. Subsystem tests arrive with the pilot migration; this file is
// not a model for them, it is the smallest proof that the frame works.
#include <ours/status.hpp>

#include <ktest/ktest.hpp>
#include <ktest/run_plan.hpp>

namespace {

/// The checks the interface offers, all passing, so a run that reaches here
/// proves the macros compile, evaluate once and reach the runtime.
KTEST(Ktest, AssertionsPass) {
    auto const value = 6 * 7;

    EXPECT_TRUE(value == 42);
    EXPECT_FALSE(value == 41);
    EXPECT_EQ(value, 42);
    EXPECT_NE(value, 41);
    EXPECT_LT(value, 43);
    EXPECT_LE(value, 42);
    EXPECT_GT(value, 41);
    EXPECT_GE(value, 42);

    auto const *const pointer = &value;
    EXPECT_NONNULL(pointer);
    EXPECT_OK(ours::Status::Ok);

    // ASSERT returns on failure, so it is only usable where the compiler can
    // see a void function; the test body is one.
    ASSERT_EQ(*pointer, 42);
}

/// A skip is reported as skipped, not as a pass and not as a failure.
KTEST(Ktest, SkipIsReported) {
    KTEST_SKIP("the interface's skip state is what this test checks");
}

/// The name carries a `_` on purpose: test names are compared as strings and
/// never go through the console command encoder, unlike a command's name.
KTEST(Ktest, Names_MayHaveUnderscores) {
    EXPECT_TRUE(true);
}

/// Run options: the first check is the fallback path, which is what `ktest
/// all` and a bare `ktest Ktest.*` use; the second only runs when the host
/// passed `expected=<n>` after the selection, so the default run stays
/// deterministic and the option path is testable from the console.
KTEST(Ktest, ReadsRunOptions) {
    EXPECT_EQ(ktest::option_usize("absent_option", usize{7}), usize{7},
              "an absent option takes its fallback");
    if (ktest::has_option("expected")) {
        EXPECT_EQ(ktest::option_usize("expected", usize{0}), usize{42},
                  "the run option is visible to the test");
    }
}

/// The matcher half of the assertion family: several EXPECT_THAT forms over
/// freestanding-safe values, all passing. The failure shape is the host
/// suite's to check exactly; this run only proves the forms compile, bind
/// their arguments once and pass on target.
KTEST(Ktest, MatcherSmoke) {
    auto const value = 6 * 7;
    EXPECT_THAT(value, ktest::Eq(42));
    EXPECT_THAT(value, ktest::Ge(42), "the answer is at least itself");
    EXPECT_THAT(value, ktest::AllOf(ktest::Ge(40), ktest::Le(45)));
    EXPECT_THAT(value, ktest::AnyOf(ktest::Eq(41), ktest::Eq(42)));

    auto const *const pointer = &value;
    EXPECT_THAT(pointer, ktest::NotNull());
    int const *const null_pointer = nullptr;
    EXPECT_THAT(null_pointer, ktest::IsNull());

    auto const status = ours::Status::Ok;
    EXPECT_THAT(status, ktest::Ok());

    ASSERT_THAT(value, ktest::Ne(41));
}

/// The runner's own options -- shuffle, repeat, shard -- reach the tests
/// through the same accessors as every other option; the iteration they plan
/// is the runner's alone. Each test below is silent unless its option was
/// given, so a plain `ktest all` keeps its summary, and each pairs with an
/// `expected_<option>` value the host passes beside it -- not
/// Ktest.ReadsRunOptions's `expected`, which names 42 and would otherwise
/// read this run's options too.

/// `ktest all shuffle[=SEED] [seed=SEED]`: the seed text arrives whole and
/// the same parser the runner uses reads it back to what the host named. A
/// bare `shuffle` carries no value and parses to the runner's default seed.
KTEST(Ktest, ReadsShuffleOption) {
    if (!ktest::has_option("shuffle")) {
        return;
    }
    auto seed = u64{0};
    EXPECT_TRUE(ktest::parse_seed(ktest::option("shuffle"), seed), "the seed text parses");
    if (ktest::has_option("seed")) {
        EXPECT_EQ(seed, ktest::option_usize("seed", usize{0}), "the seed is what the host passed");
    }
}

/// `ktest all repeat=N [expected_repeat=N]`: the pass count is visible to
/// the tests, and the run's SUMMARY shows N times the plain selection's
/// count, which is how the runner's pass loop is seen from outside.
KTEST(Ktest, ReadsRepeatOption) {
    if (!ktest::has_option("repeat")) {
        return;
    }
    auto const passes = ktest::option_usize("repeat", usize{0});
    EXPECT_GE(passes, usize{1}, "repeat carries the pass count");
    if (ktest::has_option("expected_repeat")) {
        EXPECT_EQ(passes, ktest::option_usize("expected_repeat", usize{0}),
                  "the pass count is what the host passed");
    }
}

/// `ktest all shard=M/N`: the shard text arrives whole and names a range
/// inside the registry. Which tests the shard selects is the runner's
/// arithmetic over the registry indices; the SUMMARY shows its share.
KTEST(Ktest, ReadsShardOption) {
    if (!ktest::has_option("shard")) {
        return;
    }
    auto m = usize{0};
    auto n = usize{0};
    EXPECT_TRUE(ktest::parse_shard(ktest::option("shard"), m, n), "the shard text parses");
    EXPECT_TRUE(ktest::shard_valid(m, n), "the shard names a range inside the registry");
}

} // namespace
