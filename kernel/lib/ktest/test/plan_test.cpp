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
