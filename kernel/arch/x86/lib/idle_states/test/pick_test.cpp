/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <idle_states/idle_states.hpp>

#include <string_view>

#include <gtest/gtest.h>

namespace {
using idle_states::IdleStates;
using idle_states::MwaitLeaf;
using idle_states::Signature;

constexpr auto SKYLAKE_DESKTOP = Signature{.family = 0x6, .model = 0x5e};
constexpr auto SKYLAKE_MWAIT = MwaitLeaf{.ecx = 0x3, .edx = 0x11141120};

/// Every MWAIT C-state's bit, C1 to C7.
constexpr u32 ALL = 0x7f;

/// Skylake's states, C10 890, C9 480, C8 200, C7s 124, C6 85, C3 70 and C1
/// 0 us to leave, all of them enabled unless |mask| says otherwise.
auto skylake(u32 const mask = ALL) -> IdleStates {
    auto states = IdleStates::for_cpu(
        {.intel = true, .signature = SKYLAKE_DESKTOP, .mwait = SKYLAKE_MWAIT, .arat = true});
    states.set_enabled_mask(mask);
    return states;
}

auto picked(IdleStates const &states, u64 const last_idle_us, u64 const until_deadline_us)
    -> std::string_view {
    return states.pick(last_idle_us, until_deadline_us).name;
}

constexpr auto NONE = IdleStates::NO_DEADLINE;

TEST(Pick, NoLastWaitMeansC1) {
    EXPECT_EQ(picked(skylake(), 0, NONE), "C1");
}

TEST(Pick, ALongIdleGoesDeepest) {
    EXPECT_EQ(picked(skylake(), 10'000, NONE), "C10");
}

TEST(Pick, TheDeepestStateWhoseLatencyIsUnderThreeTimesTheGuess) {
    // 3 x 100 = 300 us: C8, at 200, is the deepest under it.
    EXPECT_EQ(picked(skylake(), 100, NONE), "C8");
}

TEST(Pick, ALatencyEqualToTheBoundIsNotUnderIt) {
    // 3 x 160 = 480 us, which is C9's own latency.
    EXPECT_EQ(picked(skylake(), 160, NONE), "C8");
}

TEST(Pick, TheNextDeadlineBoundsTheGuess) {
    // The last wait says 10 ms, but a timer is due in 50 us: 3 x 50 = 150.
    EXPECT_EQ(picked(skylake(), 10'000, 50), "C7s");
}

TEST(Pick, ALaterDeadlineDoesNotLengthenTheGuess) {
    // The deadline is a bound, not a guess: 3 x 100 = 300 stands.
    EXPECT_EQ(picked(skylake(), 100, 10'000), "C8");
}

TEST(Pick, NothingDeeperFitsAndC1Is) {
    // 3 x 10 = 30 us: under every deep state's latency.
    EXPECT_EQ(picked(skylake(), 10, NONE), "C1");
}

TEST(Pick, ADeadlineThatIsDueGivesC1) {
    EXPECT_EQ(picked(skylake(), 10'000, 0), "C1");
}

TEST(Pick, AStateThatIsNotEnabledIsPassedOver) {
    // C1 and MWAIT C3, which is C6's hint 0x20: bit 2.
    EXPECT_EQ(picked(skylake(0b101), 10'000, NONE), "C6");
}

TEST(Pick, OnlyC1EnabledMeansC1) {
    EXPECT_EQ(picked(skylake(IdleStates::C1_ONLY), 10'000, NONE), "C1");
}

TEST(Pick, AHugeGuessDoesNotWrapAround) {
    // Just past a third of the range: three times it wraps to 2, under every
    // deep state's latency, where the true bound is above all of them.
    EXPECT_EQ(picked(skylake(), ~u64{0} / 3 + 1, NONE), "C10");
}

TEST(Pick, ACpuWithC1AloneAlwaysGetsC1) {
    EXPECT_EQ(picked(IdleStates(), 10'000, NONE), "C1");
}
} // namespace
