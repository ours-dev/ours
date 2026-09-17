/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for ffl::Fixed.
//
// Most of what there is to check is constexpr, so it is checked as
// static_assert: those failures are compile errors and cannot be skipped by a
// test binary nobody ran. The TESTs below carry the cases that have to run --
// values the compiler cannot see through, and the loops that would be
// unreasonable to unroll at compile time.
#include <ffl/fixed.hpp>

#include <gtest/gtest.h>

namespace {

using Q = ffl::Fixed<i64, 16>;
using Q8 = ffl::Fixed<i32, 8>;

// The defect this library exists for. As plain integers 820 / 1024 is 0, and
// a thread lighter than average accrued no weighted time at all however long
// it ran.
static_assert(820 / 1024 == 0, "the integer division this replaces");
static_assert(Q::from_ratio(820, 1024).raw() > 0);
static_assert((Q::from_ratio(820, 1024) * Q{1000}).round() == 801);

static_assert(Q{3}.raw() == 3 * 65536);
static_assert((Q{3} + Q{4}).round() == 7);
static_assert((Q{3} - Q{4}).round() == -1);
static_assert((-Q{3}).round() == -3);
static_assert((Q::from_ratio(1, 2) * Q{8}).round() == 4);
static_assert(Q::from_ratio(7, 2).floor() == 3);
static_assert(Q::from_ratio(7, 2).ceiling() == 4);
static_assert(Q::from_ratio(-7, 2).floor() == -4);
static_assert(Q::from_ratio(1, 2) < Q::from_ratio(2, 3));

// Division by zero saturates instead of trapping; see from_ratio's note.
static_assert(Q::from_ratio(1, 0).raw() == Q::max().raw());
static_assert(Q::from_ratio(-1, 0).raw() == Q::min().raw());

TEST(Fixed, RoundsHalfToEven) {
    // Ties alternate direction, which is the property that keeps a long
    // accumulation from drifting. Spelled with raw values so the halves are
    // exact rather than nearly-half.
    EXPECT_EQ(Q8::from_raw(0 * 256 + 128).round(), 0);
    EXPECT_EQ(Q8::from_raw(1 * 256 + 128).round(), 2);
    EXPECT_EQ(Q8::from_raw(2 * 256 + 128).round(), 2);
    EXPECT_EQ(Q8::from_raw(3 * 256 + 128).round(), 4);
    EXPECT_EQ(Q8::from_raw(-(1 * 256 + 128)).round(), -2);
    EXPECT_EQ(Q8::from_raw(-(2 * 256 + 128)).round(), -2);
}

TEST(Fixed, AccumulatingHalvesDoesNotDrift) {
    // The reason half-to-even was chosen. Round-half-up would bias every one
    // of these the same way and finish 1000 counts high.
    i64 rounded_sum = 0;
    for (int i = 0; i < 2000; ++i) {
        rounded_sum += Q8::from_raw(i * 256 + 128).round();
    }

    // Sum of i rounded half-to-even over [0, 2000) -- ties split evenly, so
    // the total matches the exact sum of (i + 0.5) to within one unit.
    i64 exact_doubled = 0;
    for (int i = 0; i < 2000; ++i) {
        exact_doubled += 2 * i + 1;
    }
    EXPECT_NEAR(static_cast<double>(rounded_sum), static_cast<double>(exact_doubled) / 2.0, 1.0);
}

TEST(Fixed, SaturatesInsteadOfWrapping) {
    EXPECT_EQ((Q::max() + Q::epsilon()).raw(), Q::max().raw());
    EXPECT_EQ((Q::min() - Q::epsilon()).raw(), Q::min().raw());
    EXPECT_EQ((Q::max() * Q{2}).raw(), Q::max().raw());
    EXPECT_EQ((Q::max() * Q{-2}).raw(), Q::min().raw());
}

TEST(Fixed, MultiplyKeepsPrecisionThroughTheWiderIntermediate) {
    // Both operands are large enough that their unshifted product overflows
    // i64; it is the widening that makes this answer rather than wrap.
    auto const a = Q{1 << 20};
    auto const b = Q::from_ratio(1, 4);
    EXPECT_EQ((a * b).round(), (1 << 20) / 4);
}

TEST(Fixed, RatiosOrderTheSameWayTheirValuesDo) {
    // Ordering on the raw representation is the point of a fixed-point key:
    // the scale factor is a positive constant, so it cannot reorder anything.
    for (i64 denominator = 1; denominator <= 64; ++denominator) {
        for (i64 numerator = 0; numerator < denominator; ++numerator) {
            auto const lhs = Q::from_ratio(numerator, denominator);
            auto const rhs = Q::from_ratio(numerator + 1, denominator);
            EXPECT_LT(lhs, rhs) << numerator << "/" << denominator;
        }
    }
}

} // namespace
