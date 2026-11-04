/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PHYSMAP_HPP
#define OURS_MM_PHYSMAP_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>

#include <ustl/mem/align.hpp>

namespace ours::mm {
/// The `PhysMap` represents a region of the kernel where all of useful physical memory
/// is mapped in one large chunk. It's up to the individual architecture to decide
/// how much to map but it's usually a fairly large chunk at the base of the kernel
/// address space.
struct PhysMap {
    static constexpr auto PHYS_BASE = PhysAddr{ARCH_PHYSMAP_PHYS_BASE};
    static constexpr auto VIRT_BASE = VirtAddr{ARCH_PHYSMAP_VIRT_BASE};
    static constexpr usize SIZE = ARCH_PHYSMAP_SIZE;
    static_assert(ustl::is_aligned(VIRT_BASE, MAX_PAGE_SIZE));

    static constexpr auto is_valid_phys_addr(PhysAddr const addr) -> bool {
        return addr >= PHYS_BASE && addr - PHYS_BASE < SIZE;
    }

    static constexpr auto is_valid_virt_addr(VirtAddr const addr) -> bool {
        return addr >= VIRT_BASE && addr - VIRT_BASE < SIZE;
    }

    template <typename T>
    static auto is_valid_virt_addr(T *const ptr) -> bool {
        return is_valid_virt_addr(VirtAddr{reinterpret_cast<usize>(ptr)});
    }

    static constexpr auto phys_to_virt(PhysAddr const addr) -> VirtAddr {
        if (is_valid_phys_addr(addr)) {
            return VIRT_BASE + (addr - PHYS_BASE);
        }
        return VirtAddr::Invalid;
    }

    template <typename T>
    static auto phys_to_virt(PhysAddr const addr) -> T * {
        if (!is_valid_phys_addr(addr)) {
            return nullptr;
        }
        return reinterpret_cast<T *>(std::to_underlying(phys_to_virt(addr)));
    }

    static constexpr auto virt_to_phys(VirtAddr const addr) -> PhysAddr {
        if (is_valid_virt_addr(addr)) {
            return PHYS_BASE + (addr - VIRT_BASE);
        }
        return PhysAddr::Invalid;
    }

    template <typename T>
    static auto virt_to_phys(T *const ptr) -> PhysAddr {
        return virt_to_phys(VirtAddr{reinterpret_cast<usize>(ptr)});
    }
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PHYSMAP_HPP