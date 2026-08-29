/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_PAGING_HPP
#define OURS_PHYS_ARCH_PAGING_HPP 1

#include <arch/paging/x86_pagings.hpp>

namespace ours::phys {
    using ArchLowerPaging = arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;
    using ArchUpperPaging = arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;

    constexpr PhysAddr const LOWER_MAPPING_BASE = 0;

    constexpr PhysAddr const LOWER_MAPPING_SIZE = GB(1);

    constexpr PhysAddr const UPPER_MAPPING_BASE = 0;

    constexpr PhysAddr const UPPER_MAPPING_SIZE = GB(32);

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ARCH_PAGING_HPP
