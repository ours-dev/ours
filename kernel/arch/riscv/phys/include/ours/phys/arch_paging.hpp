/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_PAGING_HPP
#define OURS_PHYS_ARCH_PAGING_HPP 1

#include <arch/paging/riscv_pagings.hpp>

namespace ours::phys {
/// Sv39 for both halves. One satp root translates the whole address space, so
/// the lower and upper schemes are the same type and Aspace keeps one root
/// (Aspace::DUAL_ASPACE is false).
using ArchLowerPaging =
    arch::paging::PagingDispatcher<arch::paging::RiscvPagingLevel::Level2>::Type;
using ArchUpperPaging =
    arch::paging::PagingDispatcher<arch::paging::RiscvPagingLevel::Level2>::Type;

/// The physical range phys maps at its own address: RAM, which on QEMU's
/// `virt` machine starts at 2 GiB, up to the 64 GiB the physmap reaches
/// (physload maps usable RAM below ARCH_PHYSMAP_SIZE identically).
constexpr PhysAddr LOWER_MAPPING_BASE = 2_gb;

constexpr PhysAddr LOWER_MAPPING_SIZE = 62_gb;

/// The physical range behind the kernel's physmap: everything from address 0,
/// mapped at the bottom of the kernel half.
constexpr PhysAddr UPPER_MAPPING_BASE = 0;

constexpr PhysAddr UPPER_MAPPING_SIZE = 64_gb;

/// An identity mapping puts a physical address at the same virtual one, and
/// Sv39's lower half ends at 256 GiB: physical memory above that cannot be
/// mapped at its own address at all.
static_assert(LOWER_MAPPING_BASE + LOWER_MAPPING_SIZE <= 256_gb,
              "The identity-mapped range fits in Sv39's lower half");

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ARCH_PAGING_HPP
