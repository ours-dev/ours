/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// ours::time::ticks_in(): what a wait written as a time comes to in counts of
// arch::Tick at a given rate, and what it answers when it cannot say.

#include <ours/time/ticks_per_second.hpp>

#include <limits>
#include <random>

#include <gtest/gtest.h>

namespace {
/// The rate ticks_in() reads. A test sets it, as the platform's setter would
/// set the kernel's.
u64 RATE = 0;

constexpr auto U64_MAX = std::numeric_limits<u64>::max();

/// Fixes the rate for the rest of the test.
auto set_rate(u64 const rate) -> void {
    RATE = rate;
}
} // namespace

// ticks_in() is inline over this one call, so a definition here is all the
// kernel's time sources it needs; the kernel's own is a load of an atomic.
namespace ours::time {
auto ticks_per_second() -> u64 {
    return RATE;
}
} // namespace ours::time

namespace {
using ours::Milliseconds;
using ours::Seconds;
using ours::time::ticks_in;

TEST(TicksIn, ADurationTakesNoCountsWhileTheRateIsUnknown) {
    set_rate(0);
    EXPECT_EQ(ticks_in(Milliseconds(250)), 0u);
    EXPECT_EQ(ticks_in(Milliseconds(2'000)), 0u);
    EXPECT_EQ(ticks_in(Milliseconds(100)), 0u);
}

// The rate QEMU's x86 measures for its 1 GHz TSC, and the three waits the
// kernel converts. The expected counts are what `rate / 1000 * milliseconds`
// gave before ticks_in() existed.
TEST(TicksIn, TheWaitsTheKernelMakesAtAQemuTscRate) {
    set_rate(1'000'005'000);
    EXPECT_EQ(ticks_in(Milliseconds(250)), 250'001'250u);
    EXPECT_EQ(ticks_in(Milliseconds(2'000)), 2'000'010'000u);
    EXPECT_EQ(ticks_in(Milliseconds(100)), 100'000'500u);
}

TEST(TicksIn, TheWaitsTheKernelMakesAtTheRiscvVirtTimebase) {
    set_rate(10'000'000);
    EXPECT_EQ(ticks_in(Milliseconds(250)), 2'500'000u);
    EXPECT_EQ(ticks_in(Milliseconds(2'000)), 20'000'000u);
    EXPECT_EQ(ticks_in(Milliseconds(100)), 1'000'000u);
}

TEST(TicksIn, AWholeSecondsDurationIsTheSameAsItsMilliseconds) {
    set_rate(2'893'000'000);
    EXPECT_EQ(ticks_in(Seconds(2)), ticks_in(Milliseconds(2'000)));
}

// The count is the rate in whole kilohertz times the milliseconds, which is
// what the callers computed before: the 999 Hz left over is dropped.
TEST(TicksIn, ARateIsRoundedDownToWholeKilohertz) {
    set_rate(1'000'999);
    EXPECT_EQ(ticks_in(Milliseconds(250)), 250'000u);
    set_rate(1'000'000);
    EXPECT_EQ(ticks_in(Milliseconds(250)), 250'000u);
}

TEST(TicksIn, ARateUnderOneKilohertzTakesNoCountsEither) {
    set_rate(999);
    EXPECT_EQ(ticks_in(Milliseconds(2'000)), 0u);
    set_rate(1'000);
    EXPECT_EQ(ticks_in(Milliseconds(2'000)), 2'000u);
}

TEST(TicksIn, ADurationThatIsNotPositiveTakesNoCounts) {
    set_rate(1'000'000'000);
    EXPECT_EQ(ticks_in(Milliseconds(0)), 0u);
    // Cast to u64, -1 ms would be the largest wait there is.
    EXPECT_EQ(ticks_in(Milliseconds(-1)), 0u);
    EXPECT_EQ(ticks_in(Milliseconds::min()), 0u);
}

// A 10 GHz counter, the fastest anyone will build for a while: a minute is
// 6e11 counts, far inside a u64, and so is a year.
TEST(TicksIn, ATenGigahertzCounterCountsAMinuteExactly) {
    set_rate(10'000'000'000);
    EXPECT_EQ(ticks_in(Milliseconds(60'000)), 600'000'000'000u);
    EXPECT_EQ(ticks_in(Seconds(365LL * 24 * 3600)), 315'360'000'000'000'000u);
}

// 10 GHz is 1e7 counts a millisecond, and a u64 holds 1.8446744e19, so the
// last millisecond count that does not overflow is 1844674407370 (a little
// over 58 years).
TEST(TicksIn, ATenGigahertzCounterSaturatesPastFiftyEightYears) {
    set_rate(10'000'000'000);
    EXPECT_EQ(ticks_in(Milliseconds(1'844'674'407'370)), 18'446'744'073'700'000'000u);
    EXPECT_EQ(ticks_in(Milliseconds(1'844'674'407'371)), U64_MAX);
    EXPECT_EQ(ticks_in(Milliseconds::max()), U64_MAX);
}

// The edge at the top of the range of rates: the largest rate has
// 18446744073709551 whole kilohertz of it, which a thousand milliseconds
// still holds (18446744073709551000) and a thousand and one do not.
TEST(TicksIn, TheLargestRateSaturatesOnTheFirstMillisecondThatDoesNotFit) {
    set_rate(U64_MAX);
    EXPECT_EQ(ticks_in(Milliseconds(1'000)), 18'446'744'073'709'551'000u);
    EXPECT_EQ(ticks_in(Milliseconds(1'001)), U64_MAX);
}

// Saturating, not wrapping: a longer wait is never fewer counts. A wrapped
// product would drop back to a few counts just past the edge.
TEST(TicksIn, ALongerDurationIsNeverFewerCounts) {
    for (auto const rate: {u64{10'000'000}, u64{1'000'005'000}, u64{10'000'000'000}, U64_MAX}) {
        set_rate(rate);
        auto previous = u64{0};
        for (auto milliseconds = Milliseconds::rep{1}; milliseconds < 1'000'000'000'000'000;
             milliseconds *= 3) {
            auto const counts = ticks_in(Milliseconds(milliseconds));
            EXPECT_GE(counts, previous) << "rate " << rate << " Hz, " << milliseconds << " ms";
            previous = counts;
        }
    }
}

// The claim of the change that introduced ticks_in(): it is the arithmetic its
// callers had, `rate / 1000 * milliseconds`, wherever that product fits in a
// u64, and the largest u64 where the old product wrapped. The cases above pin
// hand-picked values; this one pins the claim itself over rates of every
// magnitude (0 included), so a later change of the arithmetic that still
// passes those values does not go unnoticed. The seed is fixed: a failure is
// the same failure on every run.
TEST(TicksIn, TheCountIsTheOldArithmeticWhereItDidNotWrapAndSaturatedWhereItDid) {
    auto generator = std::mt19937_64{20261030};
    auto fitted = 0;
    auto wrapped = 0;
    for (auto round = 0; round < 4'000; ++round) {
        // A rate of up to `bits` bits, so the magnitudes are spread evenly
        // instead of all landing near 2^63.
        auto const bits = generator() % 65;
        auto rate = u64{0};
        if (bits != 0) {
            rate = generator() >> (64 - bits);
        }
        set_rate(rate);
        for (auto const milliseconds:
             {u64{1}, u64{100}, u64{250}, u64{2'000}, u64{60'000}, u64{86'400'000}}) {
            auto const per_millisecond = rate / 1'000;
            auto const old_count = per_millisecond * milliseconds;
            auto const old_wrapped =
                per_millisecond != 0 && old_count / per_millisecond != milliseconds;
            auto expected = old_count;
            if (old_wrapped) {
                expected = U64_MAX;
                ++wrapped;
            } else {
                ++fitted;
            }
            EXPECT_EQ(ticks_in(Milliseconds(static_cast<Milliseconds::rep>(milliseconds))),
                      expected)
                << "rate " << rate << " Hz, " << milliseconds << " ms";
        }
    }
    // Neither half of the claim may be vacuous.
    EXPECT_GT(fitted, 1'000);
    EXPECT_GT(wrapped, 1'000);
}
} // namespace
