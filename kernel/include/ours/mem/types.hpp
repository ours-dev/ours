/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_TYPES_HPP
#define OURS_MEM_TYPES_HPP 1

#include <ours/panic.hpp>
#include <ours/types.hpp>
#include <ours/marker.hpp>
#include <ours/mem/cfg.hpp>

#include <ustl/views/span.hpp>
#include <ustl/util/type-map.hpp>
#include <ustl/util/type-list.hpp>

#include <arch/paging/mmu_flags.hpp>
#include <arch/paging/controls.hpp>

#include <gktl/range.hpp>

namespace ours::mem {
    using arch::paging::MmuFlags;
    using arch::paging::MapControl;
    using arch::paging::UnmapControl;
    using arch::paging::HarvestControl;

    enum class ZoneType {
        Dma,
        Dma32 = OURS_CONFIG_ZONE_DMA ? Dma + 1 : Dma,
        Normal = OURS_CONFIG_ZONE_DMA32 ? Dma32 + 1 : Dma,
        Movable = OURS_CONFIG_ZONE_MOVABLE ? Normal + 1 : Normal,
        Dynamic = OURS_CONFIG_ZONE_DYNAMIC ? Movable + 1 : Normal,
        Count,
    };
    using ZoneValue = ustl::traits::UnderlyingTypeT<ZoneType>;

    CXX11_CONSTEXPR
    static auto NR_ZONES_PER_NODE = static_cast<usize>(ZoneType::Count);

    enum class MigrateType {
        Unmovable,
        Movable,
        Reclaimable,
        Count,
    };
    using MigrateTypeValue = ustl::traits::UnderlyingTypeT<MigrateType>;

    CXX11_CONSTEXPR
    static auto NR_MIGRATION_TYPES = static_cast<usize>(MigrateType::Count);
    
    FORCE_INLINE CXX11_CONSTEXPR 
    auto to_string(ZoneType type) -> char const * {
        switch (type) {
            case ZoneType::Dma:     return "Dma";
            case ZoneType::Dma32:   return "Dma32";
            case ZoneType::Normal:  return "Normal";
        }
        unreachable();
    }

    /// `SecNum` is a shordhand of section table entry
    /// 
    /// A address has been seen like the following layout:
    ///     | Top section index | ... | leaf section index | Control flags |
    using Pfn = usize;
    using Vpn = usize;
    using SecNum = usize;

    FORCE_INLINE CXX11_CONSTEXPR 
    auto pfn_to_phys(Pfn pfn) -> PhysAddr {  
        return PhysAddr(pfn << PAGE_SHIFT);  
    }

    template <typename T>
    FORCE_INLINE CXX11_CONSTEXPR 
    auto phys_to_pfn(T phys_addr) -> Pfn {  
        static_assert(sizeof(T) == sizeof(PhysAddr));
        return Pfn(phys_addr >> PAGE_SHIFT);  
    }

    template <typename Addr>
    FORCE_INLINE
    auto resolve_page_range(Addr base, usize size) -> gktl::Range<VirtAddr> {
        static_assert(sizeof(Addr) == sizeof(VirtAddr));
        return {
            (reinterpret_cast<VirtAddr>(base) >> PAGE_SHIFT) << PAGE_SHIFT,
            ((size + PAGE_SIZE - 1) >> PAGE_SHIFT) << PAGE_SHIFT
        };
    }

    enum class VmasFlags: usize;

    /// Forward declarations.
    /// Main classes for physical memory management.
    class PmFrame;
    class MemSection;
    class PmFolio;
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
    class VmPage;
    class VmObject;
    class VmObjectPaged;
    class VmObjectPhysical;
    class VmCowPages;
} // namespace ours::mem

/// Mark types above as kernel object to enable particular static analysis.
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::MemSection);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::PmFolio);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::PmFrame);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::PmZone);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::PmNode);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::MemCache);

OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::VmArea);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::VmAspace);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::VmObject);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::VmObjectPaged);
OURS_IMPL_MARKER_FOR(KernelObject, ours::mem::VmObjectPhysical);

#endif // #ifndef OURS_MEM_TYPES_HPP