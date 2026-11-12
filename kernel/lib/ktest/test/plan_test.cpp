/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktest/run_plan.hpp>

#include <gtest/gtest.h>

namespace {

TEST(KtestRunPlan, DefaultPlanIsThePlainRun) {
    auto const plan = ktest::RunPlan{};
    EXPECT_FALSE(plan.shuffle);
    EXPECT_EQ(plan.seed, ktest::DEFAULT_SHUFFLE_SEED);
    EXPECT_EQ(plan.repeat, usize{1});
    EXPECT_EQ(plan.shard.m, usize{0});
    EXPECT_EQ(plan.shard.n, usize{1});
    EXPECT_EQ(plan.option_count, usize{0});
}

TEST(KtestRunPlan, KnowsItsOptionNames) {
    EXPECT_EQ(ktest::runner_option("shuffle"), ktest::RunnerOption::Shuffle);
    EXPECT_EQ(ktest::runner_option("repeat"), ktest::RunnerOption::Repeat);
    EXPECT_EQ(ktest::runner_option("shard"), ktest::RunnerOption::Shard);
    EXPECT_EQ(ktest::runner_option("rounds"), ktest::RunnerOption::Other);
    EXPECT_EQ(ktest::runner_option("sharded"), ktest::RunnerOption::Other);
}

TEST(KtestRunPlan, ParsesSeeds) {
    // An empty value is the fixed default, and the one line says so both
    // ways: a refusal and a different value both fail the comparison.
    EXPECT_EQ(ktest::parse_seed("").value_or(u64{0}), ktest::DEFAULT_SHUFFLE_SEED);

    auto const decimal = ktest::parse_seed("42");
    ASSERT_TRUE(decimal.is_ok());
    EXPECT_EQ(decimal.unwrap(), u64{42});
    auto const zero = ktest::parse_seed("0");
    ASSERT_TRUE(zero.is_ok());
    EXPECT_EQ(zero.unwrap(), u64{0});

    auto const hex = ktest::parse_seed("0x10");
    ASSERT_TRUE(hex.is_ok());
    EXPECT_EQ(hex.unwrap(), u64{16});
    auto const mixed_case = ktest::parse_seed("0XaB");
    ASSERT_TRUE(mixed_case.is_ok());
    EXPECT_EQ(mixed_case.unwrap(), u64{171});
    auto const full = ktest::parse_seed("0xffffffffffffffff");
    ASSERT_TRUE(full.is_ok());
    EXPECT_EQ(full.unwrap(), ~u64{0});

    EXPECT_TRUE(ktest::parse_seed("0x").is_err());
    EXPECT_TRUE(ktest::parse_seed("1a").is_err());
    EXPECT_TRUE(ktest::parse_seed("-1").is_err());
    EXPECT_TRUE(ktest::parse_seed("0x10000000000000000").is_err());
}

TEST(KtestRunPlan, ParsesDecimals) {
    auto const zero = ktest::parse_u64("0");
    ASSERT_TRUE(zero.is_ok());
    EXPECT_EQ(zero.unwrap(), u64{0});
    auto const plain = ktest::parse_u64("61");
    ASSERT_TRUE(plain.is_ok());
    EXPECT_EQ(plain.unwrap(), u64{61});
    auto const padded = ktest::parse_u64("0061");
    ASSERT_TRUE(padded.is_ok());
    EXPECT_EQ(padded.unwrap(), u64{61});
    // The largest value the type holds parses; one past it does not.
    auto const limit = ktest::parse_u64("18446744073709551615");
    ASSERT_TRUE(limit.is_ok());
    EXPECT_EQ(limit.unwrap(), ~u64{0});
    EXPECT_TRUE(ktest::parse_u64("18446744073709551616").is_err());

    EXPECT_TRUE(ktest::parse_u64("").is_err());
    EXPECT_TRUE(ktest::parse_u64("1x").is_err());
    EXPECT_TRUE(ktest::parse_u64("+2").is_err());
}

TEST(KtestRunPlan, ParsesShards) {
    auto const half = ktest::Shard::parse("1/2");
    ASSERT_TRUE(half.is_ok());
    EXPECT_EQ(half.unwrap().m, usize{1});
    EXPECT_EQ(half.unwrap().n, usize{2});
    auto const seventh = ktest::Shard::parse("0/7");
    ASSERT_TRUE(seventh.is_ok());
    EXPECT_EQ(seventh.unwrap().m, usize{0});
    EXPECT_EQ(seventh.unwrap().n, usize{7});

    EXPECT_TRUE(ktest::Shard::parse("1").is_err());
    EXPECT_TRUE(ktest::Shard::parse("1/").is_err());
    EXPECT_TRUE(ktest::Shard::parse("/2").is_err());
    EXPECT_TRUE(ktest::Shard::parse("a/b").is_err());
}

TEST(KtestRunPlan, ValidatesShardRanges) {
    EXPECT_TRUE(ktest::Shard::parse("0/1").is_ok());
    EXPECT_TRUE(ktest::Shard::parse("1/2").is_ok());
    EXPECT_TRUE(ktest::Shard::parse("0/0").is_err());
    EXPECT_TRUE(ktest::Shard::parse("2/2").is_err());
    EXPECT_TRUE(ktest::Shard::parse("3/2").is_err());
}

TEST(KtestRunPlan, FromWordsRecordsTestOptionsAndSetsRunnerFields) {
    std::string_view const words[] = {"rounds=3", "shuffle=5", "repeat=2", "shard=1/4"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_ok());
    auto const &plan = planned.unwrap();

    EXPECT_TRUE(plan.shuffle);
    EXPECT_EQ(plan.seed, u64{5});
    EXPECT_EQ(plan.repeat, usize{2});
    EXPECT_EQ(plan.shard.m, usize{1});
    EXPECT_EQ(plan.shard.n, usize{4});
    // Every word is recorded in the order given, whatever kind it was: the
    // runner logs them all, and the tests read any of them.
    ASSERT_EQ(plan.option_count, usize{4});
    EXPECT_EQ(plan.options[0].name, "rounds");
    EXPECT_EQ(plan.options[0].value, "3");
    EXPECT_EQ(plan.options[1].name, "shuffle");
    EXPECT_EQ(plan.options[1].value, "5");
    EXPECT_EQ(plan.options[2].name, "repeat");
    EXPECT_EQ(plan.options[2].value, "2");
    EXPECT_EQ(plan.options[3].name, "shard");
    EXPECT_EQ(plan.options[3].value, "1/4");
}

TEST(KtestRunPlan, FromWordsWithNoWordsIsThePlainRun) {
    // A bare `ktest all` carries no words at all; the span is empty, not a
    // table of empty slots.
    std::string_view const *const words = nullptr;
    auto const planned = ktest::RunPlan::from_words(std::span{words, usize{0}}, true);
    ASSERT_TRUE(planned.is_ok());
    auto const &plan = planned.unwrap();
    EXPECT_FALSE(plan.shuffle);
    EXPECT_EQ(plan.repeat, usize{1});
    EXPECT_EQ(plan.option_count, usize{0});
}

TEST(KtestRunPlan, FromWordsBareShuffleRunsTheDefaultSeed) {
    std::string_view const words[] = {"shuffle"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_ok());
    EXPECT_TRUE(planned.unwrap().shuffle);
    EXPECT_EQ(planned.unwrap().seed, ktest::DEFAULT_SHUFFLE_SEED);
}

TEST(KtestRunPlan, FromWordsKeepsTestOptionsWithoutARunnerField) {
    std::string_view const words[] = {"rounds=256"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_ok());
    auto const &plan = planned.unwrap();
    EXPECT_FALSE(plan.shuffle);
    ASSERT_EQ(plan.option_count, usize{1});
    EXPECT_EQ(plan.options[0].name, "rounds");
    EXPECT_EQ(plan.options[0].value, "256");
}

TEST(KtestRunPlan, FromWordsRejectsTheWordPastTheTable) {
    std::string_view const words[ktest::MAX_RUN_OPTIONS + 1] = {};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_err());
    EXPECT_EQ(planned.unwrap_err().fault, ktest::PlanFault::TooManyWords);
}

TEST(KtestRunPlan, FromWordsRejectsAMalformedWord) {
    std::string_view const words[] = {"=value"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_err());
    auto const rejection = planned.unwrap_err();
    EXPECT_EQ(rejection.fault, ktest::PlanFault::MalformedWord);
    EXPECT_EQ(rejection.word, "=value");
}

TEST(KtestRunPlan, FromWordsRejectsRunnerOptionsOnAFilterRun) {
    std::string_view const words[] = {"shuffle=5", "repeat=2"};
    auto const planned = ktest::RunPlan::from_words(words, false);
    ASSERT_TRUE(planned.is_err());
    EXPECT_EQ(planned.unwrap_err().fault, ktest::PlanFault::RunnerWordOnSelection);
}

TEST(KtestRunPlan, FromWordsRejectsABadSeed) {
    std::string_view const words[] = {"shuffle=0x"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_err());
    auto const rejection = planned.unwrap_err();
    EXPECT_EQ(rejection.fault, ktest::PlanFault::BadSeed);
    EXPECT_EQ(rejection.word, "0x");
}

TEST(KtestRunPlan, FromWordsRejectsABadRepeat) {
    std::string_view const zero[] = {"repeat=0"};
    auto const planned_zero = ktest::RunPlan::from_words(zero, true);
    ASSERT_TRUE(planned_zero.is_err());
    auto const zero_rejection = planned_zero.unwrap_err();
    EXPECT_EQ(zero_rejection.fault, ktest::PlanFault::BadRepeat);
    EXPECT_EQ(zero_rejection.word, "0");

    std::string_view const bare[] = {"repeat"};
    auto const planned_bare = ktest::RunPlan::from_words(bare, true);
    ASSERT_TRUE(planned_bare.is_err());
    EXPECT_EQ(planned_bare.unwrap_err().fault, ktest::PlanFault::BadRepeat);
}

TEST(KtestRunPlan, FromWordsRejectsABadShard) {
    std::string_view const out_of_range[] = {"shard=2/2"};
    auto const planned = ktest::RunPlan::from_words(out_of_range, true);
    ASSERT_TRUE(planned.is_err());
    auto const rejection = planned.unwrap_err();
    EXPECT_EQ(rejection.fault, ktest::PlanFault::BadShard);
    EXPECT_EQ(rejection.word, "2/2");

    std::string_view const malformed[] = {"shard=1"};
    auto const planned_malformed = ktest::RunPlan::from_words(malformed, true);
    ASSERT_TRUE(planned_malformed.is_err());
    EXPECT_EQ(planned_malformed.unwrap_err().fault, ktest::PlanFault::BadShard);
}

TEST(KtestRunPlan, FromWordsShapesComeBeforeRunnerValues) {
    // The words are read in two passes, as the command takes them: every
    // token must be a name=value pair before any runner value is read, so a
    // malformed word at the end is the rejection, not the earlier one.
    std::string_view const words[] = {"repeat=0", "=bad"};
    auto const planned = ktest::RunPlan::from_words(words, true);
    ASSERT_TRUE(planned.is_err());
    EXPECT_EQ(planned.unwrap_err().fault, ktest::PlanFault::MalformedWord);
}

TEST(KtestRunPlan, ShardsPartitionTheRegistry) {
    // Every index belongs to exactly one shard of N: walking M = 0..N-1 sees
    // each of the count indices once, whatever N is beside it.
    auto const count = usize{25};
    for (auto const n: {usize{1}, usize{2}, usize{3}, usize{7}, usize{25}, usize{26}}) {
        auto seen = usize{0};
        for (auto m = usize{0}; m < n; ++m) {
            for (auto i = usize{0}; i < count; ++i) {
                if (ktest::in_shard(i, m, n)) {
                    seen += 1;
                }
            }
        }
        EXPECT_EQ(seen, count);
    }
}

TEST(KtestRunPlan, ShardSizesDifferByAtMostOne) {
    auto const count = usize{25};
    for (auto const n: {usize{2}, usize{3}}) {
        auto smallest = count;
        auto largest = usize{0};
        for (auto m = usize{0}; m < n; ++m) {
            auto size = usize{0};
            for (auto i = usize{0}; i < count; ++i) {
                if (ktest::in_shard(i, m, n)) {
                    size += 1;
                }
            }
            if (size < smallest) {
                smallest = size;
            }
            if (size > largest) {
                largest = size;
            }
        }
        EXPECT_EQ(largest - smallest, usize{1});
    }
}

TEST(KtestRunPlan, ShufflesDeterministically) {
    usize first[16];
    usize second[16];
    ktest::shuffle(first, 0x12345678);
    ktest::shuffle(second, 0x12345678);
    for (auto i = usize{0}; i < 16; ++i) {
        EXPECT_EQ(first[i], second[i]);
    }
}

TEST(KtestRunPlan, ShuffleIsAPermutation) {
    usize order[64];
    usize seen[64] = {};
    ktest::shuffle(order, ktest::DEFAULT_SHUFFLE_SEED);
    for (auto i = usize{0}; i < 64; ++i) {
        ASSERT_LT(order[i], usize{64});
        seen[order[i]] += 1;
    }
    for (auto i = usize{0}; i < 64; ++i) {
        EXPECT_EQ(seen[i], usize{1});
    }
}

TEST(KtestRunPlan, DifferentSeedsShuffleDifferently) {
    usize one[16];
    usize two[16];
    ktest::shuffle(one, 1);
    ktest::shuffle(two, 2);
    auto same = true;
    for (auto i = usize{0}; i < 16; ++i) {
        same = same && one[i] == two[i];
    }
    EXPECT_FALSE(same);
}

TEST(KtestRunPlan, ShufflesTinyRegistries) {
    usize order[2] = {};
    ktest::shuffle(std::span{order, usize{1}}, 42);
    EXPECT_EQ(order[0], usize{0});
    ktest::shuffle(order, 42);
    EXPECT_EQ(order[0] + order[1], usize{1});
}

} // namespace
