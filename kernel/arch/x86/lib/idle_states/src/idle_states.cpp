/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <idle_states/idle_states.hpp>

namespace idle_states {
namespace {
    /// Skylake and the client parts built on it, deepest first. The exit
    /// latencies are the documented worst cases.
    constexpr IdleState SKYLAKE[] = {
        {.name = "C10", .mwait_hint = 0x60, .exit_latency_us = 890, .flushes_tlb = true},
        {.name = "C9", .mwait_hint = 0x50, .exit_latency_us = 480, .flushes_tlb = true},
        {.name = "C8", .mwait_hint = 0x40, .exit_latency_us = 200, .flushes_tlb = true},
        {.name = "C7s", .mwait_hint = 0x33, .exit_latency_us = 124, .flushes_tlb = true},
        {.name = "C6", .mwait_hint = 0x20, .exit_latency_us = 85, .flushes_tlb = true},
        {.name = "C3", .mwait_hint = 0x10, .exit_latency_us = 70, .flushes_tlb = true},
        C1,
    };

    /// The table of an Intel family 6 part, or none.
    auto table_of(Signature const signature) -> std::span<IdleState const> {
        if (signature.family != 0x6) {
            return {};
        }
        switch (signature.model) {
            case 0x4e: // Skylake Y/U
            case 0x5e: // Skylake H/S
            case 0x8e: // Kaby Lake Y/U, Coffee Lake, Whiskey Lake, Amber Lake, Comet Lake U
            case 0x9e: // Kaby Lake H/S, Coffee Lake, Whiskey Lake
            case 0xa5: // Comet Lake S/H
                return SKYLAKE;
            default:
                return {};
        }
    }

    /// Whether MWAIT takes |hint| on a CPU reporting |mwait|: EDX valid, and
    /// the hint's sub-state among those its C-state has.
    auto mwait_takes(MwaitLeaf const mwait, u32 const hint) -> bool {
        if ((mwait.ecx & 0x1) == 0) {
            return false;
        }
        auto const substates = (mwait.edx >> (4 * mwait_cstate(hint))) & 0xf;
        return (hint & 0xf) < substates;
    }
} // namespace

auto IdleStates::for_cpu(bool const intel, Signature const signature, MwaitLeaf const mwait)
    -> IdleStates {
    auto result = IdleStates();
    if (!intel) {
        return result;
    }
    auto const table = table_of(signature);
    if (table.empty()) {
        return result;
    }

    // C1 stays whatever MWAIT says: it is the state HLT enters, and its hint
    // is the architecture's base, not something leaf 5 has to list.
    result.m_count = 0;
    for (auto const &state: table) {
        if (state.mwait_hint == C1.mwait_hint || mwait_takes(mwait, state.mwait_hint)) {
            result.m_states[result.m_count] = state;
            result.m_count += 1;
        }
    }
    return result;
}

auto IdleStates::states() const -> std::span<IdleState const> {
    return {m_states, m_count};
}

auto IdleStates::is_enabled(IdleState const &state) const -> bool {
    auto const cstate = mwait_cstate(state.mwait_hint);
    return cstate == 1 || (m_enabled_mask & (u32{1} << (cstate - 1))) != 0;
}
} // namespace idle_states
