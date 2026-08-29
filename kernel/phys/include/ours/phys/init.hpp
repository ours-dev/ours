/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_INIT_HPP
#define OURS_PHYS_INIT_HPP 1

#include <obi/container_view.hpp>
#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/phys/arch_bootmem.hpp>
#include <ours/phys/boot_package_input.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/legacy_platform_info.hpp>
#include <ours/types.hpp>

#include <span>

namespace ours::phys {
struct Handoff;
class PHYS_EXPORT Aspace;

/// physload defines all three.  A physload-loaded module declares them the
/// same way and binds to physload's objects at load time, so there is one
/// BootMem, one Handoff and one Aspace in the system rather than a copy per
/// image (ours/phys/exports.hpp).
PHYS_EXPORT extern BootMem g_bootmem;
PHYS_EXPORT extern Handoff g_handoff;
PHYS_EXPORT extern Aspace *g_aspace;

inline auto global_bootmem() -> BootMem * {
    return &g_bootmem;
}

inline auto global_handoff() -> Handoff * {
    return &g_handoff;
}

inline auto global_aspace() -> Aspace * {
    return g_aspace;
}

PHYS_EXPORT auto init_early_console() -> void;

/// Arch-implementation, WEAK_LINK default is UartKind::None.
///
/// Identifies whichever console init_early_console() just activated as the
/// debug UART, so vmours can start writing to the same already-configured
/// port immediately -- it has no way to rediscover this itself, unlike
/// ACPI-sourced facts (RSDP, MADT/CPU topology) that vmours can always
/// re-derive on its own once it has acpi_rsdp.
auto arch_uart_handoff() -> UartHandoff;

/// Arch-implementation
///
/// This method sets up architecture-specific elements after `BootMem` becomes available, such
/// as building page tables and reserving memory for special purposes. It is commonly invoked by
/// `init_memory`.
auto arch_init_memory(Aspace *aspace) -> void;

/// Arch-implementation, the `memory` OBI item's fallback.
///
/// Registers `ranges` (boot-shim's snapshot of whatever platform-specific
/// memory map it had -- see LegacyPlatformInfo) into BootMem.  The default
/// is a no-op; init_memory() only calls this when the OBI container carries
/// no typed memory item at all, so an architecture with nothing to fall
/// back on need not override it.
auto arch_detect_memory(std::span<obi::MemoryRange const> ranges) -> void;

/// First, it attempts to parse the typed `memory` OBI item. If no such item is found,
/// it invokes `arch_detect_memory` to register architecture-specific data into BootMem.
/// The outer OBI input is then reserved before any architecture setup can allocate memory.
auto init_memory(obi::ContainerView const &package, BootPackageInput const &input,
                 std::span<obi::MemoryRange const> legacy_memory_ranges, Aspace *aspace) -> void;

/// The whole reason boot-shim finds this by e_entry rather than by name:
/// physload is loaded and jumped to (never symbol-resolved) by an image
/// that predates it and cannot bind against it the way a module binds
/// against physload.  NO_MANGLE keeps ENTRY(obi_main) in phys.ld
/// unambiguous; the shim itself never spells the name at all.
///
/// `legacy` may be null: an architecture with no equivalent of Multiboot2
/// has nothing to pass.  Owned by the caller for the duration of this
/// call (obi_main never returns, so "the duration of this call" is
/// forever in practice) -- boot-shim's own stack frame is fine, since nothing
/// downstream of here switches stacks.
NO_MANGLE NO_RETURN
auto obi_main(PhysAddr package_base, usize package_size, LegacyPlatformInfo const *legacy)
    -> void;

NO_MANGLE NO_RETURN auto phys_main(PhysAddr boot_params) -> void;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_INIT_HPP
