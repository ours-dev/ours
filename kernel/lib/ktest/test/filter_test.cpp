/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktest/filter.hpp>

#include <gtest/gtest.h>

namespace {

TEST(KtestFilter, MatchesOnePattern) {
    EXPECT_TRUE(ktest::matches_pattern("Pmm.AllocatesOneFrame", "Pmm.*"));
    EXPECT_TRUE(ktest::matches_pattern("Pmm.AllocatesOneFrame", "*"));
    EXPECT_TRUE(ktest::matches_pattern("Pmm.AllocatesOneFrame", "*OneFrame"));
    EXPECT_TRUE(ktest::matches_pattern("abc", "a?c"));
    EXPECT_TRUE(ktest::matches_pattern("abc", "a*c"));
    EXPECT_TRUE(ktest::matches_pattern("", ""));
    EXPECT_FALSE(ktest::matches_pattern("Pmm.AllocatesOneFrame", "Pmm"));
    EXPECT_FALSE(ktest::matches_pattern("Pmm.AllocatesOneFrame", "Vma.*"));
    EXPECT_FALSE(ktest::matches_pattern("abc", "ab"));
    EXPECT_FALSE(ktest::matches_pattern("abc", "a?c?"));
}

TEST(KtestFilter, AppliesAColonSeparatedSelection) {
    // An empty filter is everything, and so is one that only excludes.
    EXPECT_TRUE(ktest::matches_filter("Pmm.Allocates", ""));
    EXPECT_TRUE(ktest::matches_filter("Pmm.Allocates", "-Vma.*"));
    EXPECT_FALSE(ktest::matches_filter("Pmm.Allocates", "-Pmm.*"));

    // Several including tokens, and an exclusion beside them.
    EXPECT_TRUE(ktest::matches_filter("Pmm.Allocates", "Pmm.*:Vma.*"));
    EXPECT_TRUE(ktest::matches_filter("Vma.Map", "Pmm.*:Vma.*"));
    EXPECT_FALSE(ktest::matches_filter("Semaphore.Wakes", "Pmm.*:Vma.*"));
    EXPECT_FALSE(ktest::matches_filter("Pmm.Slow", "Pmm.*:-Pmm.Slow"));
    EXPECT_TRUE(ktest::matches_filter("Pmm.Fast", "Pmm.*:-Pmm.Slow"));

    // Empty tokens are skipped rather than matching nothing.
    EXPECT_TRUE(ktest::matches_filter("Pmm.Allocates", ":Pmm.*:"));
    EXPECT_FALSE(ktest::matches_filter("Pmm.Allocates", ":Vma.*:"));
    // A filter of exclusions only includes everything that survives them.
    EXPECT_TRUE(ktest::matches_filter("Pmm.Allocates", "-Vma.*:"));
}

} // namespace
