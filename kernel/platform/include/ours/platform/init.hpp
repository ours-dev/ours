/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_INIT_HPP
#define OURS_PLATFORM_INIT_HPP 1

#include <ours/init.hpp>

#include <gktl/init_hook.hpp>

// Used by the operator+/- below; this header relied on some other include
// happening to drag it in.
#include <ustl/traits/underlying.hpp>

namespace ours {
constexpr static auto DELTA_BETWEEN_PLATFORM_INIT_LEVELS = 0x100;

enum class PlatformInitLevel {
    /// Represents the entry point for platform-specific early initialization.
    /// - **Stage Responsibility**:
    ///   Listeners registered at this level must perform *non-allocating* member initialization tasks,
    ///   focusing exclusively on foundational setup of platform-critical structures.
    ///   Strict prohibition of dynamic memory allocation ensures deterministic execution and avoids
    ///   dependency cycles during bootstrapping phases.
    /// - **Synchronization Note**:
    ///   This phase executes before memory management initialization, thus requiring lock-free
    ///   synchronization mechanisms if concurrent access is possible.
    StartUp = usize(gktl::InitLevel::PlatformEarlyInternal),

    /// Serves as the preparatory stage for Physical Memory Manager (PMM) initialization.
    /// - **Primary Objective**:
    ///   Collect platform-specific memory topology metadata required for PMM's operation, such as:
    ///   - NUMA node configurations and proximity domains
    ///   - Reserved memory regions (ACPI-defined or firmware-reported)
    ///   - Heterogeneous memory ranges (HBM, persistent memory, etc.)
    /// - **Critical Constraints**:
    ///   Must complete *before* PMM initializes its internal allocators. All gathered metadata shall be
    ///   stored in pre-allocated architecture-specific buffers to avoid premature heap usage.
    PrePmm = StartUp + DELTA_BETWEEN_PLATFORM_INIT_LEVELS,

    Pmm = PrePmm + DELTA_BETWEEN_PLATFORM_INIT_LEVELS,
};

FORCE_INLINE
constexpr static auto operator+(PlatformInitLevel level, i32 offset) -> PlatformInitLevel {
    return PlatformInitLevel(ustl::traits::UnderlyingTypeT<PlatformInitLevel>(level) + offset);
}

FORCE_INLINE
constexpr static auto operator-(PlatformInitLevel level, i32 offset) -> PlatformInitLevel {
    return PlatformInitLevel(ustl::traits::UnderlyingTypeT<PlatformInitLevel>(level) - offset);
}

INIT_CODE FORCE_INLINE
constexpr static auto set_platform_init_level(PlatformInitLevel level) -> void {
    gktl::set_init_level(gktl::InitLevel(level), DELTA_BETWEEN_PLATFORM_INIT_LEVELS);
}

/// The followings are private to platform code.

INIT_CODE
auto platform_init_pmm() -> void;

INIT_CODE
auto platform_init_keyboard() -> void;

/// The followings just be public to `start_kernel`

extern "C" INIT_CODE
auto init_platform_early() -> void;

extern "C" INIT_CODE
auto init_platform() -> void;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_INIT_HPP