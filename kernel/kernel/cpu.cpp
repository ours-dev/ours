/// SPDX-License-Identifier: MIT

#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>

#include <array>

namespace ours {

std::array<CpuMask, static_cast<usize>(Cpu::State::Count)> Cpu::g_states;

/// The CPU number lives in the per-CPU area, written when a CPU attaches to its
/// unit. It used to be kept here in a function-local `CPU_LOCAL static`, which
/// nothing outside this function could take the address of, let alone assign --
/// so every CPU read back the initialiser and `id()` was 0 everywhere.
auto Cpu::id() -> CpuNum {
    return mm::CpuLocal::current_cpu();
}

} // namespace ours
