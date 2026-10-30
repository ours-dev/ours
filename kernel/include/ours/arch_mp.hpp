/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_MP_HPP
#define OURS_ARCH_MP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/ipi_event.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <atomic>

namespace ours {
/// The architecture's half of running on several CPUs: sending one CPU's
/// interrupt to another, and taking a CPU offline and back.
///
/// Declared once here, for every architecture, because the callers are
/// generic -- IpiService, the CPU-hotplug path, the idle thread, the lockup
/// detector -- and each architecture defines the bodies in its own kernel
/// directory (x86: kernel/arch/x86/kernel/mp.cpp). What only one architecture
/// has (x86 starts its other CPUs through a real-mode trampoline) stays in
/// that architecture's own headers.

/// Raises |event| on |cpu|.
auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void;

/// Raises |event| on every CPU in |target|.
auto arch_mp_send_ipi_mask(CpuMask const &target, IpiEvent event) -> void;

/// Makes every CPU in |target| look at its run queue again. A CPU whose idle
/// state can be woken more cheaply than by an interrupt may be woken that way
/// instead.
auto arch_mp_reschedule_mask(CpuMask const &target) -> void;

/// Tells every CPU in |target| to stop for good: the architecture's half of
/// IpiService::suspend_others().
auto arch_mp_suspend(CpuMask target) -> void;

/// Sends each CPU in |targets| a non-maskable interrupt, the one interrupt a
/// CPU with interrupts off still takes. Bounded: a send the hardware never
/// completes is given up rather than waited for, because the caller is the
/// lockup detector's report, which runs with the machine already stuck.
/// Returns the CPUs it gave up on, which got nothing.
[[nodiscard]]
auto arch_mp_send_nmi(CpuMask const &targets) -> CpuMask;

/// The last thing a CPU going offline does, on itself, interrupts off, once
/// nothing in the kernel needs it any more: its caches are written back, it
/// says so in |halted|, and it halts for good. After |halted| is set it
/// takes no lock and writes nothing, so arch_deactivate_cpu() may reset it
/// from that moment on. Only with CPU hotplug.
[[noreturn]]
auto arch_cpu_halt_offline(std::atomic<bool> &halted) -> void;

/// Holds |cpu|, which arch_cpu_halt_offline() has halted, in the state it
/// can be started from again. NotSupported without CPU hotplug; BadState
/// when the hardware did not take the request.
[[nodiscard]]
auto arch_deactivate_cpu(CpuNum cpu) -> Status;

/// Starts |cpu| again, after arch_deactivate_cpu(). It comes up on its idle
/// thread's stack and is handed back to its idle thread
/// (task::IdleThread::come_back()). Returns once the start has been asked
/// for; whether the CPU arrives is for the caller to wait for. NotSupported
/// without CPU hotplug; BadState when the hardware did not take the request.
[[nodiscard]]
auto arch_activate_cpu(CpuNum cpu) -> Status;

} // namespace ours

#endif // #ifndef OURS_ARCH_MP_HPP
