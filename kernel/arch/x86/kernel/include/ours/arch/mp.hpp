/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_MP_HPP
#define OURS_ARCH_MP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/ipi_event.hpp>
#include <ours/status.hpp>

#include <atomic>

#include <ustl/views/span.hpp>

namespace ours {
namespace task {
    class Thread;
} // namespace task

auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void;

auto arch_mp_send_ipi_mask(CpuMask const &target, IpiEvent event) -> void;

auto arch_mp_reschedule(CpuNum cpu) -> void;

auto arch_mp_reschedule_mask(CpuMask const &target) -> void;

auto arch_mp_suspend(CpuMask) -> void;

/// Sends each CPU in |targets| an NMI, the one interrupt a CPU with
/// interrupts off still takes. Bounded: a send whose ICR never goes idle is
/// given up. Returns the CPUs it gave up on, which got nothing.
[[nodiscard]]
auto arch_mp_send_nmi(CpuMask const &targets) -> CpuMask;

auto x86_bring_aps(CpuMask) -> void;

extern "C" [[noreturn]] CPU_INIT_CODE USED
auto x86_start_nonboot_cpu(CpuNum cpunum, task::Thread *thread) -> void;

/// The last thing a CPU going offline does, on itself, interrupts off, once
/// nothing in the kernel needs it any more: its caches are written back, it
/// says so in |halted|, and it halts for good. After |halted| is set it
/// takes no lock and writes nothing, so arch_deactivate_cpu() may reset it
/// from that moment on. Only with CPU hotplug.
[[noreturn]]
auto arch_cpu_halt_offline(std::atomic<bool> &halted) -> void;

/// Holds |cpu|, which arch_cpu_halt_offline() has halted, in the state it
/// can be started from again: an INIT, after which it waits for a startup
/// IPI. NotSupported without CPU hotplug; BadState when the INIT was not
/// accepted.
[[nodiscard]]
auto arch_deactivate_cpu(CpuNum cpu) -> Status;

/// Starts |cpu| again through the trampoline, after arch_deactivate_cpu():
/// INIT, then two startup IPIs. It comes up on its idle thread's stack and
/// runs x86_start_nonboot_cpu(), which sets its hardware up again and hands
/// it back to its idle thread (task::IdleThread::come_back()). Returns once
/// the IPIs are sent; whether the CPU arrives is for the caller to wait for.
/// NotSupported without CPU hotplug; BadState when an IPI was not accepted.
[[nodiscard]]
auto arch_activate_cpu(CpuNum cpu) -> Status;

} // namespace ours

#endif // #ifndef OURS_ARCH_MP_HPP
