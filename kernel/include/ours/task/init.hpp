/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_INIT_HPP
#define OURS_TASK_INIT_HPP 1

namespace ours::task {
auto init_early() -> void;

auto init() -> void;

/// The part of init() only the CPU it concerns can perform, run once per CPU:
/// by init() itself for the boot CPU, and by start_nonboot_cpu() for each
/// secondary as it joins. Named after the tree's other such splits --
/// mm::CpuLocal::init_percpu(), init_local_apic_percpu(), x86_init_percpu().
auto init_percpu() -> void;

} // namespace ours::task

#endif // #ifndef OURS_TASK_INIT_HPP