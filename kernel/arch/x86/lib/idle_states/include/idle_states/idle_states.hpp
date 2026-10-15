/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IDLE_STATES_IDLE_STATES_HPP
#define IDLE_STATES_IDLE_STATES_HPP 1

#include <span>

#include <ours/types.hpp>

namespace idle_states {
/// One state an x86 CPU can wait in with MWAIT.
struct IdleState {
    char const *name;
    /// What MWAIT is given in EAX to enter it: the C-state, counted from 0
    /// for C1, in bits 7:4, and the sub-state in bits 3:0.
    u32 mwait_hint;
    /// How long the CPU takes to come out of it, in microseconds, as the
    /// part's documentation gives it. CPUID does not say.
    u32 exit_latency_us;
    /// Whether entering it may flush the TLB.
    bool flushes_tlb;
};

/// C1, the state HLT enters, and the one every CPU has: its hint is the
/// architecture's base, leaving it costs nothing to speak of, and it keeps
/// the TLB.
inline constexpr IdleState C1 = {
    .name = "C1", .mwait_hint = 0x00, .exit_latency_us = 0, .flushes_tlb = false};

/// The C-state a hint asks for, counted from 1 as CPUID leaf 5 and the
/// enabled mask count it: hint 0x00 is C1, 0x20 is C3.
[[nodiscard]]
constexpr auto mwait_cstate(u32 const hint) -> u32 {
    return ((hint >> 4) & 0xf) + 1;
}

/// What a processor is, from CPUID leaf 1's EAX: its family and model, with
/// the extended fields folded in where the architecture says they count.
struct Signature {
    u32 family;
    u32 model;
};

[[nodiscard]]
constexpr auto decode_signature(u32 const eax) -> Signature {
    auto family = (eax >> 8) & 0xf;
    auto model = (eax >> 4) & 0xf;
    if (family == 0xf) {
        family += (eax >> 20) & 0xff;
    }
    if (family == 0x6 || family >= 0xf) {
        model |= ((eax >> 16) & 0xf) << 4;
    }
    return Signature{.family = family, .model = model};
}

/// What a processor reports about MWAIT, from CPUID leaf 5: ECX bit 0 says
/// EDX is valid, and EDX gives, four bits per C-state from C0 up, how many
/// sub-states of each MWAIT can enter.
struct MwaitLeaf {
    u32 ecx;
    u32 edx;
};

/// A CPU's idle states, deepest first and C1 last, and which of them may be
/// entered.
///
/// The states come from a table per processor model, since CPUID says which
/// hints MWAIT takes but not how long each state takes to leave; a model
/// with no table has C1 alone. The table is then cut to the states the
/// CPU's MWAIT reports. Only C1 is enabled at first: a deeper state is
/// entered only once its C-state's bit is set in the mask.
class IdleStates {
  public:
    static constexpr usize MAX_STATES = 8;

    /// The mask that enables C1 alone. Bit n stands for C-state n + 1; C1's
    /// bit is ignored, C1 being always allowed.
    static constexpr u32 C1_ONLY = 0x1;

    /// C1 alone, enabled: what a CPU has before for_cpu() is asked, and
    /// what one with no table has. Constant, so that a CPU's table is valid
    /// from the first instruction.
    constexpr IdleStates() : m_states{C1}, m_count(1), m_enabled_mask(C1_ONLY) {}

    /// The states of a processor: an Intel part of |signature| gets its
    /// model's table, less the states |mwait| does not report; any other has
    /// C1 alone.
    [[nodiscard]]
    static auto for_cpu(bool intel, Signature signature, MwaitLeaf mwait) -> IdleStates;

    /// Deepest first; the last is C1.
    [[nodiscard]]
    auto states() const -> std::span<IdleState const>;

    /// Whether |state| may be entered: C1 always, another when its C-state's
    /// bit is set.
    [[nodiscard]]
    auto is_enabled(IdleState const &state) const -> bool;

    /// "No deadline": the value of |until_deadline_us| when nothing on the
    /// CPU's timer queue is due.
    static constexpr u64 NO_DEADLINE = ~u64{0};

    /// How much longer than a state's exit latency the CPU has to expect to
    /// stay idle for that state to be worth entering.
    static constexpr u64 IDLE_FACTOR = 3;

    /// The state to wait in now: the deepest enabled state whose exit latency
    /// is less than IDLE_FACTOR times the expected idle time, or C1 when none
    /// deeper is.
    ///
    /// The expected idle time is the shorter of |last_idle_us|, how long the
    /// last wait lasted, and |until_deadline_us|, how long until the next
    /// deadline on this CPU's timer queue (NO_DEADLINE when there is none).
    /// The last wait is the guess; the deadline is a bound the guess cannot
    /// pass, since the CPU is woken then whatever it guessed.
    ///
    /// With no last wait yet (|last_idle_us| zero), C1.
    [[nodiscard]]
    auto pick(u64 last_idle_us, u64 until_deadline_us) const -> IdleState const &;

    [[nodiscard]]
    auto enabled_mask() const -> u32 {
        return m_enabled_mask;
    }

    auto set_enabled_mask(u32 const mask) -> void {
        m_enabled_mask = mask;
    }

  private:
    IdleState m_states[MAX_STATES];
    usize m_count;
    u32 m_enabled_mask;
};
} // namespace idle_states

#endif // #ifndef IDLE_STATES_IDLE_STATES_HPP
