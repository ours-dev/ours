/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_PAGING_HPP
#define OURS_PHYS_ARCH_PAGING_HPP 1

/// How much of physical memory the boot shim identity-maps, from address 0,
/// before physload installs page tables of its own: start64.S fills one page
/// directory of 2 MiB entries per GiB and links each into the PDP.
///
/// physload has to write its page tables through this mapping before it can
/// switch to them, so it allocates them below this address. A #define rather
/// than a constexpr because start64.S reads it too.
#define X86_BOOT_IDENTITY_MAP_SIZE 0x100000000UL // 4 GiB

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#include <arch/paging/x86_pagings.hpp>

namespace ours::phys {
using ArchLowerPaging =
    arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;
using ArchUpperPaging =
    arch::paging::PagingDispatcher<arch::paging::X86PagingLevel::PageMapLevel4>::Type;

constexpr PhysAddr LOWER_MAPPING_BASE{};

constexpr usize LOWER_MAPPING_SIZE = 1_gb;

constexpr PhysAddr UPPER_MAPPING_BASE{};

constexpr usize UPPER_MAPPING_SIZE = 32_gb;

} // namespace ours::phys

#endif // !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#endif // #ifndef OURS_PHYS_ARCH_PAGING_HPP
