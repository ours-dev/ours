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
    EXPECT_EQ(plan.shard_index, usize{0});
    EXPECT_EQ(plan.shard_count, usize{1});
}

TEST(KtestRunPlan, KnowsItsOptionNames) {
    EXPECT_EQ(ktest::runner_option("shuffle"), ktest::RunnerOption::Shuffle);
    EXPECT_EQ(ktest::runner_option("repeat"), ktest::RunnerOption::Repeat);
    EXPECT_EQ(ktest::runner_option("shard"), ktest::RunnerOption::Shard);
    EXPECT_EQ(ktest::runner_option("rounds"), ktest::RunnerOption::Other);
    EXPECT_EQ(ktest::runner_option("sharded"), ktest::RunnerOption::Other);
}

TEST(KtestRunPlan, ParsesSeeds) {
    auto seed = u64{0};

    EXPECT_TRUE(ktest::parse_seed("", seed));
    EXPECT_EQ(seed, ktest::DEFAULT_SHUFFLE_SEED);

    EXPECT_TRUE(ktest::parse_seed("42", seed));
    EXPECT_EQ(seed, u64{42});
    EXPECT_TRUE(ktest::parse_seed("0", seed));
    EXPECT_EQ(seed, u64{0});

    EXPECT_TRUE(ktest::parse_seed("0x10", seed));
    EXPECT_EQ(seed, u64{16});
    EXPECT_TRUE(ktest::parse_seed("0XaB", seed));
    EXPECT_EQ(seed, u64{171});
    EXPECT_TRUE(ktest::parse_seed("0xffffffffffffffff", seed));
    EXPECT_EQ(seed, ~u64{0});

    EXPECT_FALSE(ktest::parse_seed("0x", seed));
    EXPECT_FALSE(ktest::parse_seed("1a", seed));
    EXPECT_FALSE(ktest::parse_seed("-1", seed));
    EXPECT_FALSE(ktest::parse_seed("0x10000000000000000", seed));
}

TEST(KtestRunPlan, ParsesDecimals) {
    auto value = usize{0};

    EXPECT_TRUE(ktest::parse_decimal("0", value));
    EXPECT_EQ(value, usize{0});
    EXPECT_TRUE(ktest::parse_decimal("61", value));
    EXPECT_EQ(value, usize{61});
    EXPECT_TRUE(ktest::parse_decimal("0061", value));
    EXPECT_EQ(value, usize{61});

    EXPECT_FALSE(ktest::parse_decimal("", value));
    EXPECT_FALSE(ktest::parse_decimal("1x", value));
    EXPECT_FALSE(ktest::parse_decimal("+2", value));
}

TEST(KtestRunPlan, ParsesShards) {
    auto m = usize{0};
    auto n = usize{0};

    EXPECT_TRUE(ktest::parse_shard("1/2", m, n));
    EXPECT_EQ(m, usize{1});
    EXPECT_EQ(n, usize{2});
    EXPECT_TRUE(ktest::parse_shard("0/7", m, n));
    EXPECT_EQ(m, usize{0});
    EXPECT_EQ(n, usize{7});

    EXPECT_FALSE(ktest::parse_shard("1", m, n));
    EXPECT_FALSE(ktest::parse_shard("1/", m, n));
    EXPECT_FALSE(ktest::parse_shard("/2", m, n));
    EXPECT_FALSE(ktest::parse_shard("a/b", m, n));
}

TEST(KtestRunPlan, ValidatesShardRanges) {
    EXPECT_TRUE(ktest::shard_valid(0, 1));
    EXPECT_TRUE(ktest::shard_valid(1, 2));
    EXPECT_FALSE(ktest::shard_valid(0, 0));
    EXPECT_FALSE(ktest::shard_valid(2, 2));
    EXPECT_FALSE(ktest::shard_valid(3, 2));
}

TEST(KtestRunPlan, ShardsPartitionTheRegistry) {
    // Every index belongs to exactly one shard of N: walking M = 0..N-1 sees
    // each of the count indices once, whatever N is beside it.
    auto const count = usize{25};
    for (auto const n: {usize{1}, usize{2}, usize{3}, usize{7}, usize{25}, usize{26}}) {
        auto seen = usize{0};
        for (auto m = usize{0}; m < n; ++m) {
            ASSERT_TRUE(ktest::shard_valid(m, n));
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
    auto const count = usize{16};
    usize first[16];
    usize second[16];
    ktest::shuffle_indices(first, count, 0x12345678);
    ktest::shuffle_indices(second, count, 0x12345678);
    for (auto i = usize{0}; i < count; ++i) {
        EXPECT_EQ(first[i], second[i]);
    }
}

TEST(KtestRunPlan, ShuffleIsAPermutation) {
    auto const count = usize{64};
    usize order[64];
    usize seen[64] = {};
    ktest::shuffle_indices(order, count, ktest::DEFAULT_SHUFFLE_SEED);
    for (auto i = usize{0}; i < count; ++i) {
        ASSERT_LT(order[i], count);
        seen[order[i]] += 1;
    }
    for (auto i = usize{0}; i < count; ++i) {
        EXPECT_EQ(seen[i], usize{1});
    }
}

TEST(KtestRunPlan, DifferentSeedsShuffleDifferently) {
    usize one[16];
    usize two[16];
    ktest::shuffle_indices(one, 16, 1);
    ktest::shuffle_indices(two, 16, 2);
    auto same = true;
    for (auto i = usize{0}; i < 16; ++i) {
        same = same && one[i] == two[i];
    }
    EXPECT_FALSE(same);
}

TEST(KtestRunPlan, ShufflesTinyRegistries) {
    usize order[2] = {};
    ktest::shuffle_indices(order, 1, 42);
    EXPECT_EQ(order[0], usize{0});
    ktest::shuffle_indices(order, 2, 42);
    EXPECT_EQ(order[0] + order[1], usize{1});
}

} // namespace
