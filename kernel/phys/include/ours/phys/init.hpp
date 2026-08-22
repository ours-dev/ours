/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_INIT_HPP
#define OURS_PHYS_INIT_HPP 1

#include <obi/container-view.hpp>
#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/phys/arch-bootmem.hpp>
#include <ours/phys/boot-package-input.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/types.hpp>

namespace ours::phys {
struct Handoff;
class Aspace;

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

/// The global BootMem wrapped as the type-erased EarlyMem service that
/// crosses the physload→module and phys→kernel handoff boundaries.
PHYS_EXPORT auto early_mem() -> EarlyMem;

/// Arch-implementation
///
/// This method sets up architecture-specific elements after `BootMem` becomes available, such
/// as building page tables and reserving memory for special purposes. It is commonly invoked by
/// `init_memory`.
auto arch_init_memory(Aspace *aspace) -> void;

/// First, it attempts to parse the typed `memory` OBI item. If no such item is found,
/// it invokes `arch_detect_memory` to register architecture-specific data into BootMem.
/// The outer OBI input is then reserved before any architecture setup can allocate memory.
auto init_memory(obi::ContainerView const &package, BootPackageInput const &input, Aspace *aspace)
    -> void;

NO_RETURN
auto obi_main(PhysAddr package_base, usize package_size) -> void;

NO_MANGLE NO_RETURN auto phys_main(PhysAddr boot_params) -> void;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_INIT_HPP
