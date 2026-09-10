/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/bitfields.hpp>

#include <tuple>

#include <cassert>
#include <cstdint>

namespace {

enum class Mode : std::uint8_t {
    Idle = 0,
    Run = 3,
};

enum FieldId {
    MODE,
    COUNT,
    DISABLED,
    READY,
};

using State = ustl::BitFields<
    ustl::Field<ustl::bitfields::Id<MODE>, ustl::bitfields::Bits<2>, ustl::bitfields::Type<Mode>>,
    ustl::Field<ustl::bitfields::Id<COUNT>, ustl::bitfields::Bits<3>,
                ustl::bitfields::Type<std::uint8_t>>,
    ustl::Field<ustl::bitfields::Id<DISABLED>, ustl::bitfields::Bits<5>,
                ustl::bitfields::Type<std::uint8_t>, ustl::bitfields::Enable<false>>,
    ustl::Field<ustl::bitfields::Id<READY>, ustl::bitfields::Bits<1>, ustl::bitfields::Type<bool>>>;

using WideState =
    ustl::BitFields<ustl::bitfields::StorageUnit<std::uint64_t>,
                    std::tuple<ustl::Field<ustl::bitfields::Id<0>, ustl::bitfields::Bits<16>,
                                           ustl::bitfields::Type<std::uint16_t>>,
                               ustl::Field<ustl::bitfields::Id<1>, ustl::bitfields::Bits<48>,
                                           ustl::bitfields::Type<std::uintptr_t>>>>;

enum AddrFieldId {
    FLAG,
    ADDR,
};

// Mirrors a page-table-base-style field (e.g. x86 CR3's page table address):
// bits [12, 52) hold a 4k-aligned address whose value is read/written
// in-place, not shifted down to bit 0.
using AddrState = ustl::BitFields<
    ustl::Field<ustl::bitfields::Id<FLAG>, ustl::bitfields::Bits<1>, ustl::bitfields::Type<bool>>,
    ustl::Field<ustl::bitfields::Id<ADDR>, ustl::bitfields::Bits<40>, ustl::bitfields::StartBit<12>,
                ustl::bitfields::Type<std::uint64_t>, ustl::bitfields::Natural<true>>>;

enum class FlagBit : std::uint8_t { Lo = 0, Hi = 5 };

// FieldAt derives StartBit from the Id itself (Lo=0 -> bit 0, Hi=5 -> bit 5),
// so there's exactly one place — the enum — that says where each bit lives.
using FlagState = ustl::BitFields<ustl::FieldAt<FlagBit::Lo>, ustl::FieldAt<FlagBit::Hi>>;

static_assert(sizeof(State) == sizeof(std::uint8_t));
static_assert(sizeof(WideState) == sizeof(std::uint64_t));
static_assert(State::NUM_BITS == 6);
static_assert(!State::IsFieldEnabledV<DISABLED>);

constexpr auto test_field_at() -> bool {
    auto flags = FlagState::from_raw(0).set<FlagBit::Lo>(true);
    return flags.get<FlagBit::Lo>() && !flags.get<FlagBit::Hi>() &&
           flags.into<std::uint8_t>() == 0b1;
}

static_assert(test_field_at());

constexpr auto test_natural_field() -> bool {
    AddrState addr;
    addr.set<FLAG>(true).set<ADDR>(0x1234'5678'9000ULL);
    return addr.get<ADDR>() == 0x1234'5678'9000ULL && addr.get<FLAG>() &&
           addr.into<std::uint64_t>() == 0x1234'5678'9001ULL &&
           AddrState::from_raw(0x1234'5678'9000ULL).get<ADDR>() == 0x1234'5678'9000ULL;
}

static_assert(test_natural_field());

constexpr auto test_constexpr_state() -> bool {
    State state;
    state.set<MODE>(Mode::Run).set<COUNT>(7).set<READY>(true);
    return state.get<MODE>() == Mode::Run && state.get<COUNT>() == 7 && state.get<READY>() &&
           state.get<DISABLED>() == 0 && state.into<std::uint8_t>() == 0x3f;
}

static_assert(test_constexpr_state());

} // namespace

auto main() -> int {
    State state;
    state.set<COUNT>(0xff);
    state.set<READY>(true);
    assert(state.get<COUNT>() == 7);
    assert(state.get<READY>());

    volatile State device_state;
    device_state.set<MODE>(Mode::Run);
    device_state.set<READY>(true);
    assert(device_state.get<MODE>() == Mode::Run);
    assert(device_state.get<READY>());

    int value = 7;
    WideState wide;
    wide.set<0>(0xabcd);
    wide.set<1>(reinterpret_cast<std::uintptr_t>(&value) >> 16);
    assert(wide.get<0>() == 0xabcd);
    assert(wide.get<1>() == (reinterpret_cast<std::uintptr_t>(&value) >> 16));
}
