/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_INIT_HPP
#define OURS_ARCH_X86_INIT_HPP 1

#include <ours/types.hpp>

namespace ours {
    /// Called once on BSP, but it is a temporary work around. 
    auto x86_init_idt_early() -> void;
    auto x86_init_mmu_early() -> void;
    auto x86_init_feature_early() -> void;

    /// Called once on BSP, and rest of APs just call x86_load_*dt(); 
    auto x86_setup_idt() -> void;
    auto x86_setup_gdt() -> void;

    /// Called once on each CPU
    auto x86_setup_tss_percpu(CpuNum cpunum) -> void;
    auto x86_init_mmu_percpu() -> void;
    auto x86_init_feature_percpu(CpuNum cpunum) -> void;

    /// Called in init_arch_early()
    auto x86_setup_mmu() -> void;

    auto x86_init_percpu(CpuNum cpunum) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_INIT_HPP