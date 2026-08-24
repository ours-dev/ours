/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_HOOK_HPP
#define GKTL_HOOK_HPP 1

#include <gktl/types.hpp>

#include <utility>

#define GKTL_INIT_HOOK(NAME, HOOK, LEVEL)                                                          \
    LINK_SECTION(".gktl.init_hook")                                                                \
    FORCE_USED constexpr static gktl::InitHook const GKTL_INIT_HOOK_##NAME = {                     \
        HOOK, static_cast<gktl::InitLevel>(LEVEL), #NAME};

namespace gktl {
constexpr auto LEVEL_DELTA = 0x10000_i32;

/// Trace the the initialization level of system during boot.
enum class InitLevel {
    LowestLevel,
    ArchEarlyInternal = LowestLevel,
    ArchEarly = ArchEarlyInternal + LEVEL_DELTA,

    PlatformEarlyInternal = ArchEarly + LEVEL_DELTA,
    PlatformEarly = PlatformEarlyInternal + LEVEL_DELTA,

    HeapInternal = PlatformEarly + LEVEL_DELTA,
    Heap = HeapInternal + LEVEL_DELTA,

    VmmInternal = Heap + LEVEL_DELTA,
    Vmm = VmmInternal + LEVEL_DELTA,

    CpuLocalInternal = Vmm + LEVEL_DELTA,
    CpuLocal = CpuLocalInternal + LEVEL_DELTA,

    IrqInternal = CpuLocal + LEVEL_DELTA,
    Irq = IrqInternal + LEVEL_DELTA,

    TimeInternal = Irq + LEVEL_DELTA,
    Time = TimeInternal + LEVEL_DELTA,

    ThreadingInternal = Time + LEVEL_DELTA,
    Threading = ThreadingInternal + LEVEL_DELTA,

    ArchInternal = Threading + LEVEL_DELTA,
    Arch = ArchInternal + LEVEL_DELTA,

    PlatformInternal = Arch + LEVEL_DELTA,
    Platform = PlatformInternal + LEVEL_DELTA,

    MaxInitLevel = Platform + LEVEL_DELTA,
    None,
};

FORCE_INLINE
constexpr auto operator+(InitLevel level, i32 offset) -> InitLevel {
    return static_cast<InitLevel>((std::to_underlying(level) + offset));
}

FORCE_INLINE
constexpr auto operator-(InitLevel level, i32 offset) -> InitLevel {
    return static_cast<InitLevel>((std::to_underlying(level) - offset));
}

struct InitHook {
    using Fn = auto (*)() -> void;

    Fn fn;
    InitLevel level;
    char const *name;
};

/// Setting a level causes all hooks with levels between the previous level and the
/// newly set |level| (exclusive) to be executed.
auto set_init_level(InitLevel level, i32 delta = LEVEL_DELTA) -> void;

FORCE_INLINE
inline auto get_init_level() -> InitLevel {
    extern InitLevel g_init_level;
    return g_init_level;
}

} // namespace gktl

#endif // GKTL_HOOK_HPP