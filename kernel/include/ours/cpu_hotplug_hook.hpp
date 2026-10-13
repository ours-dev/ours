/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_HOTPLUG_HOOK_HPP
#define OURS_CPU_HOTPLUG_HOOK_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>

namespace ours {
/// The stage of a CPU's bring-up a hotplug hook belongs to, in the order a
/// CPU coming back sets them up. Going offline runs them the other way round,
/// so each level's teardown finds every lower level still standing.
enum class CpuHotplugLevel : u8 {
    /// The CPU's tick and timer device: up first, so that the levels above
    /// may arm timers; down last, once nothing above is left to need it.
    Tick,
    /// Per-CPU timers of their own: armed once the tick runs again, and
    /// cancelled before it stops, so that none is among the timers another
    /// CPU takes over.
    Timers,
    /// The CPU's interprocessor-call mailbox: open last, once the CPU can
    /// serve a request; closed first, right after the CPU leaves the online
    /// mask.
    Ipi,
    Count,
};

/// One subsystem's part in a CPU's going offline and coming back. Both halves
/// run on the CPU changing state, in its idle thread, interrupts off, with
/// nothing else on that CPU running: |offline| as it goes, |online| as it
/// comes back. Either may be null. A subsystem registers one with
/// CPU_HOTPLUG_HOOK() in its own file, and the CPU hotplug path walks them
/// all (task::run_offline_hooks(), task::run_online_hooks()), knowing none
/// of them by name.
struct CpuHotplugHook {
    using Fn = auto (*)() -> void;
    CpuHotplugLevel level;
    Fn offline;
    Fn online;
    char const *name;
};
} // namespace ours

/// Registers a CpuHotplugHook named NAME at LEVEL. Collected by the linker
/// into one resident table, which nothing references but the walk: hence
/// the section and USED.
#define CPU_HOTPLUG_HOOK(NAME, LEVEL, OFFLINE, ONLINE)                                             \
    LINK_SECTION(".cpu_hotplug.hooks")                                                             \
    USED constexpr static ::ours::CpuHotplugHook const CPU_HOTPLUG_HOOK_##NAME = {LEVEL, OFFLINE,  \
                                                                                  ONLINE, #NAME};

#endif // #ifndef OURS_CPU_HOTPLUG_HOOK_HPP
