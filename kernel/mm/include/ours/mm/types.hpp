/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_TYPES_HPP
#define OURS_MM_TYPES_HPP 1

#include <arch/paging/controls.hpp>
#include <arch/paging/mmu_flags.hpp>
#include <gktl/range.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/panic.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
using arch::paging::HarvestControl;
using arch::paging::MapControl;
using arch::paging::MmuFlags;
using arch::paging::UnmapControl;

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

FORCE_INLINE constexpr auto pfn_to_phys(Pfn pfn) -> PhysAddr {
    return PhysAddr(pfn << PAGE_SHIFT);
}

template <typename T>
FORCE_INLINE constexpr auto phys_to_pfn(T phys_addr) -> Pfn {
    static_assert(sizeof(T) == sizeof(PhysAddr));
    return Pfn(phys_addr >> PAGE_SHIFT);
}

template <typename Addr>
FORCE_INLINE auto resolve_page_range(Addr base, usize size) -> gktl::Range<VirtAddr> {
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
class VmMapping;
using VmPage = PageFrame;
class VmObject;
class VmObjectPaged;
class VmObjectPhysical;
class VmCowPages;
} // namespace ours::mm

#endif // #ifndef OURS_MM_TYPES_HPP
