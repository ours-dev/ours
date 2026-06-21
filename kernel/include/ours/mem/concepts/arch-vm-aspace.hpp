/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_CONCEPTS_ARCH_VM_ASPACE_HPP
#define OURS_MEM_CONCEPTS_ARCH_VM_ASPACE_HPP 1

#include <ours/mem/types.hpp>

namespace ours::mem::concepts {
    using ustl::traits::IsSameV;

    template <typename T>
    concept ArchVmAspace = requires {
        { IsSameV<decltype(&T::init), auto(T::*)()->Status> };
        { IsSameV<decltype(&T::map), auto(T::*)(VirtAddr, PhysAddr, usize, MmuFlags, MapControl, usize *)->Status> };
        { IsSameV<decltype(&T::map_bulk), auto(T::*)(VirtAddr, PhysAddr *, usize, MmuFlags, MapControl, usize *)->Status> };
        { IsSameV<decltype(&T::unmap), auto(T::*)(VirtAddr, PhysAddr *, usize, MmuFlags, MapControl, usize *)->Status> };
        { IsSameV<decltype(&T::protect), auto(T::*)(VirtAddr, usize, MmuFlags)->Status> };
        { IsSameV<decltype(&T::query), auto(T::*)(VirtAddr, PhysAddr *, MmuFlags *)->Status> };
        { IsSameV<decltype(&T::mark_accessed), auto(T::*)(VirtAddr, usize)->Status> };
        { IsSameV<decltype(&T::harvest_accessed), auto(T::*)(VirtAddr, usize, HarvestControl)->Status> };
    };

} // namespace ours::mem::concepts

#endif // #ifndef OURS_MEM_CONCEPTS_ARCH_VM_ASPACE_HPP