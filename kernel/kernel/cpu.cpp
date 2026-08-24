/// SPDX-License-Identifier: MIT

#include <ours/cpu.hpp>

#include <ours/mm/cpu-local.hpp>

namespace ours {

ustl::Array<CpuMask, static_cast<usize>(Cpu::State::Count)> Cpu::g_states;

auto Cpu::id() -> CpuNum {
    CPU_LOCAL static CpuNum current_cpunum = 0;
    return mm::CpuLocal::read(current_cpunum);
}

auto Cpu::nid() -> NnId {
    CPU_LOCAL static NnId current_nid = 0;
    return mm::CpuLocal::read(current_nid);
}

}  // namespace ours
