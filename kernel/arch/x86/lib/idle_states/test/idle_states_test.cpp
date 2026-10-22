/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <idle_states/idle_states.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {
using idle_states::decode_signature;
using idle_states::IdleStates;
using idle_states::mwait_cstate;
using idle_states::MwaitLeaf;
using idle_states::Processor;
using idle_states::Signature;

/// Leaf 5 of a Skylake desktop part: ECX bit 0 set, and from C0 up 0, 2, 1,
/// 1, 4, 1, 1, 1 sub-states.
constexpr auto SKYLAKE_MWAIT = MwaitLeaf{.ecx = 0x3, .edx = 0x11141120};

/// What QEMU's leaf 5 answers, whatever the model: EDX valid and empty.
constexpr auto QEMU_MWAIT = MwaitLeaf{.ecx = 0x3, .edx = 0x0};

constexpr auto SKYLAKE_DESKTOP = Signature{.family = 0x6, .model = 0x5e};

/// A Skylake desktop part as CPUID describes it, with |mwait| for its leaf 5.
constexpr auto skylake(MwaitLeaf const mwait) -> Processor {
    return Processor{.intel = true, .signature = SKYLAKE_DESKTOP, .mwait = mwait, .arat = true};
}

auto names(IdleStates const &states) -> std::string {
    auto out = std::string();
    for (auto const &state: states.states()) {
        out += out.empty() ? "" : " ";
        out += state.name;
    }
    return out;
}

TEST(IdleStates, HintsCountCStatesFromOne) {
    EXPECT_EQ(mwait_cstate(0x00), 1u);
    EXPECT_EQ(mwait_cstate(0x10), 2u);
    EXPECT_EQ(mwait_cstate(0x20), 3u);
    EXPECT_EQ(mwait_cstate(0x33), 4u);
    EXPECT_EQ(mwait_cstate(0x60), 7u);
}

TEST(IdleStates, SignaturesFoldTheExtendedModel) {
    // QEMU's Nehalem and Skylake-Client, as their leaf 1 reads.
    auto const nehalem = decode_signature(0x106a3);
    EXPECT_EQ(nehalem.family, 0x6u);
    EXPECT_EQ(nehalem.model, 0x1au);
    auto const skylake = decode_signature(0x506e3);
    EXPECT_EQ(skylake.family, 0x6u);
    EXPECT_EQ(skylake.model, 0x5eu);
}

TEST(IdleStates, BeforeAnythingIsKnownThereIsC1Alone) {
    auto const states = IdleStates();
    EXPECT_EQ(names(states), "C1");
    EXPECT_TRUE(states.is_enabled(states.states().back()));
}

TEST(IdleStates, AKnownPartGetsItsTableDeepestFirst) {
    auto const states = IdleStates::for_cpu(skylake(SKYLAKE_MWAIT));
    EXPECT_EQ(names(states), "C10 C9 C8 C7s C6 C3 C1");
    EXPECT_EQ(states.states().front().exit_latency_us, 890u);
    EXPECT_EQ(std::string_view(states.states().back().name), "C1");
}

TEST(IdleStates, AStateMwaitDoesNotReportIsLeftOut) {
    // C4 with three sub-states: C7s, sub-state 3, is not among them.
    auto const mwait = MwaitLeaf{.ecx = 0x3, .edx = 0x11131120};
    EXPECT_EQ(names(IdleStates::for_cpu(skylake(mwait))), "C10 C9 C8 C6 C3 C1");
}

TEST(IdleStates, UnderQemuOnlyC1IsLeft) {
    EXPECT_EQ(names(IdleStates::for_cpu(skylake(QEMU_MWAIT))), "C1");
}

TEST(IdleStates, WithoutAValidLeafFiveOnlyC1IsLeft) {
    auto const mwait = MwaitLeaf{.ecx = 0x2, .edx = 0x11141120};
    EXPECT_EQ(names(IdleStates::for_cpu(skylake(mwait))), "C1");
}

TEST(IdleStates, AnUnknownModelOrAnotherVendorHasC1Alone) {
    auto nehalem = skylake(SKYLAKE_MWAIT);
    nehalem.signature = Signature{.family = 0x6, .model = 0x1a};
    EXPECT_EQ(names(IdleStates::for_cpu(nehalem)), "C1");

    auto other_vendor = skylake(SKYLAKE_MWAIT);
    other_vendor.intel = false;
    EXPECT_EQ(names(IdleStates::for_cpu(other_vendor)), "C1");
}

TEST(IdleStates, WithoutAratOnlyC1IsLeft) {
    // Every deeper state would stop the timer that wakes the CPU.
    auto no_arat = skylake(SKYLAKE_MWAIT);
    no_arat.arat = false;
    EXPECT_EQ(names(IdleStates::for_cpu(no_arat)), "C1");
}

TEST(IdleStates, OnlyC1IsEnabledUntilTheMaskSaysOtherwise) {
    auto states = IdleStates::for_cpu(skylake(SKYLAKE_MWAIT));
    EXPECT_EQ(states.enabled_mask(), IdleStates::C1_ONLY);
    for (auto const &state: states.states()) {
        EXPECT_EQ(states.is_enabled(state), mwait_cstate(state.mwait_hint) == 1) << state.name;
    }

    // C3's hint is MWAIT C2, C6's is MWAIT C3: bits 1 and 2.
    states.set_enabled_mask(0b110);
    for (auto const &state: states.states()) {
        auto const cstate = mwait_cstate(state.mwait_hint);
        EXPECT_EQ(states.is_enabled(state), cstate <= 3) << state.name;
    }
}
} // namespace
