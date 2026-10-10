/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_PAGING_HPP
#define OURS_PHYS_ARCH_PAGING_HPP 1

#include <arch/paging/x86_pagings.hpp>

namespace ours::phys {
using ArchLowerPaging =
    arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;
using ArchUpperPaging =
    arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;

constexpr PhysAddr LOWER_MAPPING_BASE = 0;

constexpr PhysAddr LOWER_MAPPING_SIZE = 1_gb;

constexpr PhysAddr UPPER_MAPPING_BASE = 0;

constexpr PhysAddr UPPER_MAPPING_SIZE = 32_gb;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ARCH_PAGING_HPP
