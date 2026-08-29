/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PHYSMAP_HPP
#define OURS_MM_PHYSMAP_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>

namespace ours::mm {
/// The `PhysMap` represents a region of the kernel where all of useful physical memory
/// is mapped in one large chunk. It's up to the individual architecture to decide
/// how much to map but it's usually a fairly large chunk at the base of the kernel
/// address space.
struct PhysMap {
    constexpr static PhysAddr kPhysBase = ARCH_PHYSMAP_PHYS_BASE;

    constexpr static VirtAddr kVirtBase = ARCH_PHYSMAP_VIRT_BASE;
    static_assert(!(kVirtBase & ((MAX_PAGE_SIZE)-1)), "");

    constexpr static usize const kSize = ARCH_PHYSMAP_SIZE;

    template <typename T>
    FORCE_INLINE constexpr static auto is_valid_phys_addr(T phys_addr) -> bool {
        static_assert(sizeof(usize) == sizeof(T));
        return kPhysBase <= reinterpret_cast<PhysAddr>(phys_addr) &&
               reinterpret_cast<PhysAddr>(phys_addr) - kPhysBase < kSize;
    }

    template <typename T>
    FORCE_INLINE constexpr static auto is_valid_virt_addr(T virt_addr) -> bool {
        static_assert(sizeof(usize) == sizeof(T));
        return kVirtBase <= reinterpret_cast<VirtAddr>(virt_addr) &&
               reinterpret_cast<VirtAddr>(virt_addr) - kVirtBase < kSize;
    }

    template <typename T>
    FORCE_INLINE constexpr static auto phys_to_virt(T phys_addr) -> VirtAddr {
        static_assert(sizeof(usize) == sizeof(T));
        if (PhysMap::is_valid_phys_addr(phys_addr)) {
            return VirtAddr(kVirtBase + (phys_addr - kPhysBase));
        }
        return VirtAddr();
    }

    template <typename U, typename T>
    FORCE_INLINE constexpr static auto phys_to_virt(T phys_addr) -> U * {
        static_assert(sizeof(usize) == sizeof(T));
        return reinterpret_cast<U *>(PhysMap::phys_to_virt(phys_addr));
    }

    template <typename T>
    FORCE_INLINE constexpr static auto virt_to_phys(T virt_addr) -> PhysAddr {
        static_assert(sizeof(usize) == sizeof(T));
        if (PhysMap::is_valid_virt_addr(virt_addr)) {
            return PhysAddr(kPhysBase + (reinterpret_cast<VirtAddr>(virt_addr) - kVirtBase));
        }
        return PhysAddr();
    }

    template <typename U, typename T>
    FORCE_INLINE constexpr static auto virt_to_phys(T virt_addr) -> U * {
        static_assert(sizeof(usize) == sizeof(T));
        return reinterpret_cast<U *>(PhysMap::virt_to_phys(virt_addr));
    }
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PHYSMAP_HPP