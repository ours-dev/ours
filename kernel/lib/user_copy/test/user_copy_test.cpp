/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <user_copy/range.hpp>

#include <limits>

#include <gtest/gtest.h>

namespace {
using user_copy::is_within;

constexpr ours::VirtAddr BASE = 0x20'0000;
constexpr usize SPAN = (usize{1} << 47) - 0x1000 - BASE;

TEST(IsWithin, WholeRangeInside) {
    EXPECT_TRUE(is_within(BASE, 0x1000, BASE, SPAN));
    EXPECT_TRUE(is_within(BASE + SPAN - 1, 1, BASE, SPAN));
}

TEST(IsWithin, EndsExactlyAtTheLimit) {
    EXPECT_TRUE(is_within(BASE + SPAN - 0x1000, 0x1000, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + SPAN - 0x1000, 0x1001, BASE, SPAN));
}

TEST(IsWithin, NullAndTheHoleBelowTheBase) {
    EXPECT_FALSE(is_within(0, 1, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE - 1, 1, BASE, SPAN));
    EXPECT_FALSE(is_within(0, 0, BASE, SPAN));
}

TEST(IsWithin, KernelHalf) {
    EXPECT_FALSE(is_within(0xffff'8000'0000'0000, 1, BASE, SPAN));
}

TEST(IsWithin, SizeThatWouldWrapTheSum) {
    // `addr + size` wraps to a small number here; a naive `end <= limit` check
    // would accept it.
    EXPECT_FALSE(is_within(BASE + 0x1000, std::numeric_limits<usize>::max(), BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + 0x1000, std::numeric_limits<usize>::max() - 0xfff, BASE, SPAN));
}

TEST(IsWithin, EmptyRangeMustStillStartInside) {
    EXPECT_TRUE(is_within(BASE, 0, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + SPAN, 0, BASE, SPAN));
}

} // namespace
