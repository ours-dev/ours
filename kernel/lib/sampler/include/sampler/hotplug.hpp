/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_HOTPLUG_HPP
#define SAMPLER_HOTPLUG_HPP 1

namespace sampler {
/// The sampling session's side of a CPU going offline and coming back
/// (task::take_cpu_offline(), task::bring_cpu_online()), its per-CPU timer
/// cancelled on the way down and armed on the way back up, as perf's CPU
/// hotplug callbacks do.
///
/// The thread that drives the change holds the sessions' control lock across
/// it, from begin_hotplug() to end_hotplug(), so that no session starts or
/// stops halfway: a start would arm a CPU that was about to leave, or a stop
/// miss one that was just back. Between the two, the CPU changing state runs
/// on_cpu_offline() or on_cpu_online() on itself.
///
/// Lock order: the hotplug lock, then this one.
auto begin_hotplug() -> void;
auto end_hotplug() -> void;

/// On the CPU going offline, interrupts off, before its tick stops: its
/// sampling timer is cancelled, so it is not among the timers another CPU
/// takes over.
auto on_cpu_offline() -> void;

/// On the CPU coming back, interrupts off, once its tick runs again: its
/// sampling timer is armed if a session is running.
auto on_cpu_online() -> void;
} // namespace sampler

#endif // #ifndef SAMPLER_HOTPLUG_HPP
