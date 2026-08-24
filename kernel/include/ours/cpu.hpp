/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_HPP
#define OURS_CPU_HPP 1

#include <ours/cpu-mask.hpp>
#include <ours/types.hpp>

#include <ustl/array.hpp>

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
    static auto nid() -> NnId;

    [[nodiscard]] static auto is_online(CpuNum cpu) -> bool {
        return state_mask(State::Online).test(cpu);
    }

    static auto set_state(CpuNum cpu, State state, bool enabled = true) -> void {
        state_mask(state).set(cpu, enabled);
    }

    static auto set_state(State state, bool enabled = true) -> void {
        set_state(id(), state, enabled);
    }

    [[nodiscard]] static auto possible_mask() -> CpuMask & {
        return state_mask(State::Possible);
    }

    [[nodiscard]] static auto present_mask() -> CpuMask & {
        return state_mask(State::Present);
    }

    [[nodiscard]] static auto online_mask() -> CpuMask & {
        return state_mask(State::Online);
    }

    [[nodiscard]] static auto num_possible_cpus() -> usize {
        return possible_mask().count();
    }

    [[nodiscard]] static auto num_present_cpus() -> usize {
        return present_mask().count();
    }

    [[nodiscard]] static auto num_online_cpus() -> usize {
        return online_mask().count();
    }

  private:
    static constexpr auto kNumStates = static_cast<usize>(State::Count);

    [[nodiscard]] static auto state_mask(State state) -> CpuMask & {
        return g_states[static_cast<usize>(state)];
    }

    static ustl::Array<CpuMask, kNumStates> g_states;
};

inline auto cpu_possible_mask() -> CpuMask & {
    return Cpu::possible_mask();
}

}  // namespace ours

#endif  // OURS_CPU_HPP
