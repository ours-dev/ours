/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_HPP
#define OURS_CPU_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours {

class Cpu {
  public:
    enum class State : usize {
        Possible,
        Present,
        Online,
        Count,
    };

    static auto id() -> CpuNum;

    /// For the node a CPU belongs to, use mm::PmNode::Current::nid(). `Cpu::nid()`
    /// used to duplicate it from a function-local `CPU_LOCAL static` that could
    /// never be written, so it always answered 0; it had no callers.

    [[nodiscard]]
    static auto is_online(CpuNum cpu) -> bool {
        return state_mask(State::Online).test(cpu);
    }

    static auto set_state(CpuNum cpu, State state, bool enabled = true) -> void {
        state_mask(state).set(cpu, enabled);
    }

    static auto set_state(State state, bool enabled = true) -> void {
        set_state(id(), state, enabled);
    }

    [[nodiscard]]
    static auto possible_mask() -> CpuMask & {
        return state_mask(State::Possible);
    }

    [[nodiscard]]
    static auto present_mask() -> CpuMask & {
        return state_mask(State::Present);
    }

    [[nodiscard]]
    static auto online_mask() -> CpuMask & {
        return state_mask(State::Online);
    }

    [[nodiscard]]
    static auto num_possible_cpus() -> usize {
        return possible_mask().count();
    }

    [[nodiscard]]
    static auto num_present_cpus() -> usize {
        return present_mask().count();
    }

    [[nodiscard]]
    static auto num_online_cpus() -> usize {
        return online_mask().count();
    }

  private:
    static constexpr auto NUM_STATES = static_cast<usize>(State::Count);

    [[nodiscard]]
    static auto state_mask(State state) -> CpuMask & {
        return STATES[static_cast<usize>(state)];
    }

    static std::array<CpuMask, NUM_STATES> STATES;
};

inline auto cpu_possible_mask() -> CpuMask & {
    return Cpu::possible_mask();
}

/// The two constant masks, Linux's cpu_none_mask and cpu_all_mask.
///
/// They are returned by reference to a single static object because the
/// callers that want them keep a pointer -- a ClockEventDevice records
/// `CpuMask const *` -- and a per-call temporary would dangle. cpu_all_mask()
/// is "every CPU the kernel was built for", the bitset's full capacity, and
/// is deliberately wider than cpu_possible_mask(), which is what the
/// firmware said is present on this machine.
inline auto cpu_none_mask() -> CpuMask const & {
    static constexpr CpuMask NONE{};
    return NONE;
}

inline auto cpu_all_mask() -> CpuMask const & {
    static constexpr CpuMask ALL = CpuMask{}.set();
    return ALL;
}

/// The mask holding |cpunum| and nothing else, Linux's cpumask_of().
///
/// A table rather than a constructed temporary for the same reason the two
/// constants above are: the affinity a thread carries is a non-owning
/// `CpuMask const *`, so anything installed as one has to outlive the
/// installer. Pinning a thread to a single CPU -- which is what an idle
/// thread is -- has no other way to be expressed.
///
/// The table is one CpuMask per CPU the kernel was built for, so it is sized
/// by MAX_CPU rather than by what the firmware reported; the indexes past
/// cpu_possible_mask() are never asked for, and making it dynamic would cost
/// an allocation to save a few words.
inline auto cpu_mask_of(CpuNum cpunum) -> CpuMask const & {
    static constexpr auto SINGLETONS = [] {
        std::array<CpuMask, MAX_CPU> masks{};
        for (auto i = usize(0); i < MAX_CPU; ++i) {
            masks[i].set(i);
        }
        return masks;
    }();

    return SINGLETONS[cpunum];
}

} // namespace ours

#endif // OURS_CPU_HPP
