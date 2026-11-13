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

TEST(KtestSummary, CountsEveryExecutionUnderRepeat) {
    // repeat=N runs the selection N times, and the SUMMARY counts
    // executions: the same test's outcome is recorded once per pass, so one
    // test can contribute a skip in one pass and a pass in another.
    auto summary = ktest::Summary{};
    for (auto pass = usize{0}; pass < 2; ++pass) {
        summary.record(false, false);
        summary.record(false, true);
    }
    EXPECT_EQ(summary.ran, usize{4});
    EXPECT_EQ(summary.passed, usize{2});
    EXPECT_EQ(summary.skipped, usize{2});
    EXPECT_EQ(summary.failed, usize{0});
}

} // namespace
