/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_CPU_HOTPLUG_HPP
#define OURS_TASK_CPU_HOTPLUG_HPP 1

#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
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
/// Step 2 is bounded by |deadline| too: it takes as long as the CPU takes to
/// reschedule, which a thread spinning there with preemption off can put
/// off indefinitely. A CPU that has not taken the transition up by then is
/// left as it was, its Dpc workers started again, and TimeOut is answered.
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
/// stopped; TimeOut from step 2, as above.
[[nodiscard]]
auto take_cpu_offline(CpuNum cpu, time::Deadline deadline) -> Status;

/// Brings |cpu| back: it is started again through the AP trampoline
/// (arch_activate_cpu()), its idle thread takes back up what it put down, in
/// reverse (IdleThread::come_back()), and its Dpc workers start again.
/// InvArgs for a CPU that is not possible; NotSupported without CPU hotplug;
/// BadState unless it is offline, or when the start-up IPIs were not taken;
/// TimeOut when it did not arrive within a second. It is then left offline,
/// and halts again should it arrive later, so a retry starts it afresh.
[[nodiscard]]
auto bring_cpu_online(CpuNum cpu) -> Status;

/// The lock a CPU's going offline or coming back holds throughout. A
/// subsystem whose control path arms or disarms something on every online
/// CPU takes it first, before any lock of its own, so that no CPU changes
/// state halfway through: the sampling sessions' start and stop do.
[[nodiscard]]
auto cpu_hotplug_lock() -> sync::Mutex &;

/// Every registered CpuHotplugHook's offline half, from the highest level to
/// the lowest, and within a level last registered first; then, coming back,
/// every online half the other way round. On the CPU changing state, in its
/// idle thread, interrupts off (IdleThread::go_offline_and_halt(),
/// IdleThread::come_back()).
auto run_offline_hooks() -> void;
auto run_online_hooks() -> void;
} // namespace ours::task

#endif // #ifndef OURS_TASK_CPU_HOTPLUG_HPP
