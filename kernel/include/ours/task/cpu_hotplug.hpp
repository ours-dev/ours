/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_CPU_HOTPLUG_HPP
#define OURS_TASK_CPU_HOTPLUG_HPP 1

#include <ours/status.hpp>
#include <ours/time/timer.hpp>
#include <ours/types.hpp>

namespace ours::task {
/// Takes |cpu| offline, from any CPU, |cpu| included. In order:
///
///   1. its Dpc workers stop (DpcRunner::stop_workers()), within |deadline|;
///   2. its idle thread takes it offline (IdleThread::transition()): its
///      queued threads move to other active CPUs, it leaves the active and
///      online masks, closes its IPI mailbox, stops its tick, and waits with
///      interrupts off;
///   3. it halts, and is held in INIT (arch_deactivate_cpu());
///   4. the calling CPU takes over its timers and its queued Dpcs.
///
/// Once asked, the CPU goes offline, so step 2 is waited for without a
/// deadline; it takes as long as the CPU takes to reschedule.
///
/// No thread may be left that can run only on |cpu|, beyond its idle thread
/// and Dpc workers: one that is queued stays queued until the CPU is back,
/// and one that wakes while it is offline fails selection's assertion.
///
/// Errors: InvArgs for a CPU that is not possible; NotSupported without CPU
/// hotplug (the build argument cpu_hotplug) and for the CPU that owns
/// jiffies, which is the boot CPU, and takes every device interrupt;
/// BadState when |cpu| is not online, or no other CPU is active to take its
/// threads; stop_workers()'s TimeOut, with the workers that did stop left
/// stopped.
[[nodiscard]]
auto take_cpu_offline(CpuNum cpu, time::Deadline deadline) -> Status;

/// Brings |cpu| back: it is started again through the AP trampoline
/// (arch_activate_cpu()), its idle thread takes back up what it put down, in
/// reverse (IdleThread::come_back()), and its Dpc workers start again.
/// InvArgs for a CPU that is not possible; NotSupported without CPU hotplug;
/// BadState unless it is offline, or when the start-up IPIs were not taken;
/// TimeOut when it did not arrive within a second, after which it may still.
[[nodiscard]]
auto bring_cpu_online(CpuNum cpu) -> Status;
} // namespace ours::task

#endif // #ifndef OURS_TASK_CPU_HOTPLUG_HPP
