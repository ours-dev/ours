/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktest/summary.hpp>

#include <gtest/gtest.h>

namespace {

TEST(KtestSummary, CountsOutcomes) {
    auto summary = ktest::Summary{};
    summary.record(false, false);
    summary.record(false, true);
    summary.record(true, false);
    // A test that failed is counted as failed even when it also asked to be
    // skipped, because the failure is what the run must report.
    summary.record(true, true);

    EXPECT_EQ(summary.ran, usize{4});
    EXPECT_EQ(summary.passed, usize{1});
    EXPECT_EQ(summary.skipped, usize{1});
    EXPECT_EQ(summary.failed, usize{2});
}

TEST(KtestSummary, StartsAtZero) {
    auto const summary = ktest::Summary{};
    EXPECT_EQ(summary.ran, usize{0});
    EXPECT_EQ(summary.passed, usize{0});
    EXPECT_EQ(summary.failed, usize{0});
    EXPECT_EQ(summary.skipped, usize{0});
}

} // namespace
