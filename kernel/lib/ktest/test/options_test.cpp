/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktest/options.hpp>

#include <gtest/gtest.h>

namespace {

TEST(KtestOptions, ParsesNameValueAndPresence) {
    auto option = ktest::Option{};

    EXPECT_TRUE(ktest::parse_option("rounds=256", option));
    EXPECT_EQ(option.name, "rounds");
    EXPECT_EQ(option.value, "256");

    EXPECT_TRUE(ktest::parse_option("big", option));
    EXPECT_EQ(option.name, "big");
    EXPECT_TRUE(option.value.empty());

    EXPECT_TRUE(ktest::parse_option("name=", option));
    EXPECT_EQ(option.name, "name");
    EXPECT_TRUE(option.value.empty());

    EXPECT_TRUE(ktest::parse_option("a=b=c", option));
    EXPECT_EQ(option.name, "a");
    EXPECT_EQ(option.value, "b=c");

    EXPECT_FALSE(ktest::parse_option("=value", option));
}

TEST(KtestOptions, FindsTheFirstMatch) {
    ktest::Option const entries[] = {
        {"rounds", "16"},
        {"big", ""},
        {"rounds", "256"},
    };
    auto const *const found = ktest::find_option(entries, 3, "rounds");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, "16");
    EXPECT_EQ(ktest::find_option(entries, 3, "absent"), nullptr);
}

} // namespace
