/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_INIT_HPP
#define OURS_MM_INIT_HPP 1

#include <ours/init.hpp>
#include <ours/mm/init_stage.hpp>
#include <ours/mm/types.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/status.hpp>

#include <gktl/range.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
enum class InitContext {
    Early,
    Hotplug,
};

INIT_CODE
auto handoff_early_pmm(phys::MemoryHandoff const &) -> Status;

/// Perform the initialization of physical memory manager. Call this function after
/// invoking the `setup_nodes`.
///
/// |max_zone_pfn| holds one bound for each zone type, at the type's index
/// (ZoneType): the first pfn past that type's memory, or 0 for a type the
/// platform has none of. The types are laid end to end from the lowest usable
/// pfn, each up to its bound.
INIT_CODE
auto init_pmm(ustl::views::Span<Pfn> max_zone_pfn = {}) -> void;

/// Perform the initialization of the object caches and kmalloc -- the kernel
/// object heap. Requires init_pmm() and CpuLocal::init(), in that order.
///
/// Not to be confused with the VMM's own heap (vmalloc), which is a separate
/// step much later in init_vmm().
INIT_CODE
auto init_cache() -> void;

/// Perform the initialization of virtual memory manager.
INIT_CODE
auto init_vmm() -> void;

enum class InitArea {
    All,
};
/// Deliberately not INIT_CODE: it reclaims the `.init*` sections, so it must
/// not live in them. See EarlyMem::reclaim_init_area().
auto free_init_area(InitArea = InitArea::All) -> void;

} // namespace ours::mm

#endif // #ifndef OURS_MM_INIT_HPP
