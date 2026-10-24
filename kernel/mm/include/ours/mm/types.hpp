/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_TYPES_HPP
#define OURS_MM_TYPES_HPP 1

#include <ours/mm/cfg.hpp>
#include <ours/panic.hpp>
#include <ours/types.hpp>

#include <arch/paging/controls.hpp>
#include <arch/paging/mmu_flags.hpp>
#include <gktl/range.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
// Qualified from the global namespace on purpose. Inside `namespace ours::mm`
// an unqualified `arch::` finds `ours::arch` first, and `ours::arch` really
// does exist -- ours/mm/cfg.hpp declares it. This compiled before only
// because <arch/paging/...> was included ahead of every ours/ header, so
// `ours::arch` had not been declared yet at this point in the file. That is
// not something an include order should be load-bearing for.
using ::arch::paging::HarvestOptions;
/// The page table's own choices for a map, under another name in mm: mm's
/// MapOptions (vm_mapping.hpp) is what VmMapping::map()'s callers choose, in
/// mm's terms, and only mm's own code talks to the page table.
using ArchMapOptions = ::arch::paging::MapOptions;
using ::arch::paging::MmuFlags;
using ::arch::paging::UnmapOptions;

enum class ZoneType {
    Dma,
    Dma32,
    Normal,
    Movable,
    Dynamic,
    Count,
};

constexpr auto NR_ZONES_PER_NODE = static_cast<usize>(ZoneType::Count);

enum class MigrateType {
    Unmovable,
    Movable,
    Reclaimable,
    Count,
};

constexpr auto NR_MIGRATION_TYPES = static_cast<usize>(MigrateType::Count);

/// `SecNum` is a shordhand of section table entry
///
/// A address has been seen like the following layout:
///     | Top section index | ... | leaf section index | Control flags |
using Pfn = usize;
using Vpn = usize;
using SecNum = usize;

FORCE_INLINE
constexpr auto pfn_to_phys(Pfn pfn) -> PhysAddr {
    return PhysAddr(pfn << PAGE_SHIFT);
}

template <typename T>
FORCE_INLINE
constexpr auto phys_to_pfn(T phys_addr) -> Pfn {
    static_assert(sizeof(T) == sizeof(PhysAddr));
    return Pfn(phys_addr >> PAGE_SHIFT);
}

template <typename Addr>
FORCE_INLINE
auto resolve_page_range(Addr base, usize size) -> gktl::Range<VirtAddr> {
    static_assert(sizeof(Addr) == sizeof(VirtAddr));
    return {(reinterpret_cast<VirtAddr>(base) >> PAGE_SHIFT) << PAGE_SHIFT,
            ((size + PAGE_SIZE - 1) >> PAGE_SHIFT) << PAGE_SHIFT};
}

enum class VmasFlags : usize;

/// Forward declarations.
/// Main classes for physical memory management.
///
/// PmFrame/VmPage/PmFolio are aliases of PageFrame (pm_frame.hpp), so they are
/// forward-declared through it rather than as classes of their own.
class PageFrame;
using PmFrame = PageFrame;
using PmFolio = PageFrame;
using Slab = PageFrame;
class MemSection;
class PmZone;
class PmNode;
class MemCache;

/// Auxiliary classes for physical memory management.
class FrameQueue;

/// Main classes for virtual memory management.
class ArchVmAspace;
class VmAspace;
class VmArea;
class VmAreaOrMapping;
class VmMapping;
using VmPage = PageFrame;
class VmObject;
class VmObjectPaged;
class VmObjectPhysical;
class VmCowPages;

/// What an operation on a range of a node's pages does to the page tables of
/// every mapping of them, of every object over the node
/// (VmCowPages::change_range_locked()). Done under the node's lock, so that
/// no fault can map the pages again before the operation is over.
enum class RangeChange : u8 {
    /// Write comes off the entries, which keep Read and Execute; the mappings'
    /// records keep Write, so that a write faults and is resolved. For pages
    /// about to be shared: a clone.
    RemoveWrite,
    /// The entries go; the mappings' records stay, so that the next access
    /// faults and maps whatever the node holds then. For pages about to leave
    /// the node: take_pages().
    Unmap,
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_TYPES_HPP
