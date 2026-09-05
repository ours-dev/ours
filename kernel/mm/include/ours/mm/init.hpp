/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_INIT_HPP
#define OURS_MM_INIT_HPP 1

#include <gktl/range.hpp>
#include <ours/init.hpp>
#include <ours/mm/init_stage.hpp>
#include <ours/mm/types.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/status.hpp>
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
/// |zpis| gives out of the priority topology among non-overlapping memory
/// regions. Such as the following example showed:
///     [0x0, 0xFFFF]           MemoryPriority::Low,
///     [0x10000, 0x1FFFF]      MemoryPriority::Normal,
///     [0x20000, 0x3FFFF]      MemoryPriority::High,
///     [0x40000, 0x5FFFF]      MemoryPriority::Critical,
///     [0x60000, 0x7FFFF]      MemoryPriority::Normal,
/// But it is not required,
///
INIT_CODE
auto init_pmm(ustl::views::Span<Pfn> pfn = {}) -> void;

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
