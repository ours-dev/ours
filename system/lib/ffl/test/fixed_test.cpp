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
#include <ffl/mul_shr.hpp>

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

// --- Cross-format conversion ---------------------------------------------
//
// Widening the fraction is exact; narrowing it rounds. Nothing here may be
// implicit, which is why each conversion is spelled with a cast.
static_assert(Q{ffl::Fixed<i64, 8>::from_ratio(1, 2)}.raw() == Q::from_ratio(1, 2).raw());
static_assert(ffl::Fixed<i64, 8>{Q::from_ratio(3, 512)}.raw() == 2);

static_assert((Q{9} / i64{2}).raw() == Q::from_ratio(9, 2).raw());
static_assert((-Q{3}).absolute().raw() == Q{3}.raw());
// The most negative value has no positive counterpart; saturating is the only
// answer that is still non-negative.
static_assert(Q::min().absolute().raw() == Q::max().raw());

// --- mul_shr --------------------------------------------------------------
//
// The two variants differ only in the discarded bits, and that difference is
// the entire reason both exist: pin it so neither can quietly become the
// other.
constexpr auto R3122 = ffl::reciprocal<u64>(3122, 32);
static_assert(ffl::mul_shr<u64>(1000 * 3122, R3122, 32) == 999);
static_assert(ffl::mul_shr_round<u64>(1000 * 3122, R3122, 32) == 1000);
static_assert(ffl::mul_shr_round<u64>(1000 * 820, ffl::reciprocal<u64>(1024, 32), 32) == 801);
static_assert(ffl::mul_shr_round<u64>(5, 7, 0) == 35);

TEST(MulShr, MatchesDivisionAcrossAWideRangeOfWeights) {
    // The claim being tested is that multiply-by-reciprocal can stand in for
    // the division it replaces. "Stand in" means within one unit: the
    // reciprocal is itself rounded, so exact agreement is not available and
    // demanding it would only be testing the test.
    for (u64 weight = 1; weight <= 70000; weight += 137) {
        auto const wmult = ffl::reciprocal<u64>(weight, 32);
        for (u64 delta = 1; delta <= 1'000'000; delta *= 10) {
            auto const exact = delta * 3122 / weight;
            auto const scaled = ffl::mul_shr_round<u64>(delta * 3122, wmult, 32);
            auto const error = exact > scaled ? exact - scaled : scaled - exact;
            EXPECT_LE(error, exact / 1000 + 1) << delta << " * 3122 / " << weight;
        }
    }
}

TEST(MulShr, TruncationBiasIsWhatRoundingRemoves) {
    // Truncation errs downward every time, so its error accumulates instead
    // of cancelling. Summing the same scaled value many times is the cheapest
    // way to make that visible.
    //
    // 20/3 is chosen deliberately: its fractional part is above a half, so
    // the two variants land on different integers. A value whose fraction is
    // below a half rounds down either way and would show nothing.
    constexpr u64 SHIFT = 32;
    constexpr u64 ITERATIONS = 10'000;
    constexpr u64 VALUE = 20;
    constexpr u64 DIVISOR = 3;
    auto const wmult = ffl::reciprocal<u64>(DIVISOR, SHIFT);

    u64 truncated = 0;
    u64 rounded = 0;
    for (u64 i = 0; i < ITERATIONS; ++i) {
        truncated += ffl::mul_shr<u64>(VALUE, wmult, SHIFT);
        rounded += ffl::mul_shr_round<u64>(VALUE, wmult, SHIFT);
    }

    constexpr u64 EXACT = ITERATIONS * VALUE / DIVISOR;
    EXPECT_LT(truncated, EXACT);
    EXPECT_LT(EXACT - truncated, ITERATIONS);

    auto const rounding_error = rounded > EXACT ? rounded - EXACT : EXACT - rounded;
    EXPECT_LT(rounding_error, EXACT - truncated);
}

} // namespace
