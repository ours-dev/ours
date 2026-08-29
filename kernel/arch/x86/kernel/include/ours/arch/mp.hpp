/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_MP_HPP
#define OURS_ARCH_MP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/ipi_event.hpp>
#include <ustl/views/span.hpp>

namespace ours {
auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void;

auto arch_mp_send_ipi_mask(CpuMask const &target, IpiEvent event) -> void;

auto arch_mp_reschedule(CpuNum cpu) -> void;

auto arch_mp_reschedule_mask(CpuMask const &target) -> void;

auto arch_mp_suspend(CpuMask) -> void;

auto x86_bring_aps(CpuMask) -> void;

NO_MANGLE NO_RETURN INIT_CODE USED auto x86_start_nonboot_cpu(CpuNum cpunum) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_MP_HPP
