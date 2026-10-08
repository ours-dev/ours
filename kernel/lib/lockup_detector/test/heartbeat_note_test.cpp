/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// The check a CPU makes of another CPU's heartbeat once a second: when a
// target counts as stuck, and what resets the count.

#include <lockup_detector/heartbeat_note.hpp>

#include <gtest/gtest.h>

namespace {
using lockup_detector::HeartbeatNote;

constexpr auto THRESHOLD = u32{10};

TEST(HeartbeatNote, ATargetThatTicksIsNeverStuck) {
    auto note = HeartbeatNote{};
    for (auto heartbeat = u64{1}; heartbeat <= 100; ++heartbeat) {
        EXPECT_FALSE(note.check(heartbeat * 100, THRESHOLD));
        EXPECT_EQ(note.unchanged(), 0u);
    }
}

TEST(HeartbeatNote, StuckOnTheThresholdsCheckWithoutATick) {
    auto note = HeartbeatNote{};
    EXPECT_FALSE(note.check(42, THRESHOLD));
    for (auto check = u32{1}; check < THRESHOLD; ++check) {
        EXPECT_FALSE(note.check(42, THRESHOLD)) << "check " << check;
        EXPECT_EQ(note.unchanged(), check);
    }
    EXPECT_TRUE(note.check(42, THRESHOLD));
    EXPECT_EQ(note.unchanged(), THRESHOLD);
}

TEST(HeartbeatNote, OneTickStartsTheCountOver) {
    auto note = HeartbeatNote{};
    EXPECT_FALSE(note.check(7, THRESHOLD));
    for (auto check = u32{1}; check < THRESHOLD; ++check) {
        EXPECT_FALSE(note.check(7, THRESHOLD));
    }
    EXPECT_FALSE(note.check(8, THRESHOLD));
    EXPECT_EQ(note.unchanged(), 0u);
    for (auto check = u32{1}; check < THRESHOLD; ++check) {
        EXPECT_FALSE(note.check(8, THRESHOLD));
    }
    EXPECT_TRUE(note.check(8, THRESHOLD));
}

TEST(HeartbeatNote, ATargetThatNeverTickedIsNotCounted) {
    auto note = HeartbeatNote{};
    for (auto check = u32{0}; check < 3 * THRESHOLD; ++check) {
        EXPECT_FALSE(note.check(0, THRESHOLD));
    }
    EXPECT_EQ(note.unchanged(), 0u);
}

TEST(HeartbeatNote, ThresholdZeroNeverFindsOne) {
    auto note = HeartbeatNote{};
    for (auto check = u32{0}; check < 3 * THRESHOLD; ++check) {
        EXPECT_FALSE(note.check(5, 0));
    }
    EXPECT_EQ(note.unchanged(), 3 * THRESHOLD - 1);
}

TEST(HeartbeatNote, StaysStuckPastTheThreshold) {
    auto note = HeartbeatNote{};
    EXPECT_FALSE(note.check(3, 2));
    EXPECT_FALSE(note.check(3, 2));
    EXPECT_TRUE(note.check(3, 2));
    EXPECT_TRUE(note.check(3, 2));
    EXPECT_EQ(note.unchanged(), 3u);
}

} // namespace
