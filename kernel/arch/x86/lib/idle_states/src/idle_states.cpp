/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <idle_states/idle_states.hpp>

#include <algorithm>

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

/// Without ARAT every state deeper than C1 would stop the timer that wakes
/// the CPU for its own deadlines, so C1 is all such a CPU gets: the deeper
/// states would need the deadline handed to a device that keeps running
/// while the CPU sleeps, which nothing here does.
auto IdleStates::for_cpu(Processor const &processor) -> IdleStates {
    auto result = IdleStates();
    if (!processor.intel || !processor.arat) {
        return result;
    }
    auto const table = table_of(processor.signature);
    if (table.empty()) {
        return result;
    }

    // C1 stays whatever MWAIT says: it is the state HLT enters, and its hint
    // is the architecture's base, not something leaf 5 has to list.
    result.m_count = 0;
    for (auto const &state: table) {
        if (state.mwait_hint == C1.mwait_hint || mwait_takes(processor.mwait, state.mwait_hint)) {
            result.m_states[result.m_count] = state;
            result.m_count += 1;
        }
    }
    return result;
}

/// The table is deepest first, so the first state that is enabled and short
/// enough to leave is the deepest such. C1, last, is never tested: it is the
/// answer when nothing deeper fits, enabled or not.
auto IdleStates::pick(u64 const last_idle_us, u64 const until_deadline_us) const
    -> IdleState const & {
    auto const &c1 = m_states[m_count - 1];
    if (last_idle_us == 0) {
        return c1;
    }

    auto const expected = std::min(last_idle_us, until_deadline_us);
    // IDLE_FACTOR * expected wraps for an expected time past a third of the
    // range, and a wrapped bound would turn the deepest state away. Any
    // latency is under a bound that large.
    auto const huge = expected > ~u64{0} / IDLE_FACTOR;
    for (usize i = 0; i + 1 < m_count; ++i) {
        auto const &state = m_states[i];
        if (is_enabled(state) && (huge || state.exit_latency_us < IDLE_FACTOR * expected)) {
            return state;
        }
    }
    return c1;
}

auto IdleStates::states() const -> std::span<IdleState const> {
    return {m_states, m_count};
}

auto IdleStates::is_enabled(IdleState const &state) const -> bool {
    auto const cstate = mwait_cstate(state.mwait_hint);
    return cstate == 1 || (m_enabled_mask & (u32{1} << (cstate - 1))) != 0;
}
} // namespace idle_states
