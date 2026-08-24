/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ASPACE_LAYOUT_HPP
#define OURS_ARCH_ASPACE_LAYOUT_HPP 1

#include <ours/macro_abi.hpp>

#define KERNEL_ASPACE_BASE  0xffffff8000000000UL
#define KERNEL_ASPACE_SIZE  GB(512)

#define ARCH_PHYSMAP_PHYS_BASE  0 
#define ARCH_PHYSMAP_VIRT_BASE  KERNEL_ASPACE_BASE
#define ARCH_PHYSMAP_SIZE       GB(32)

/// Virtual window the per-CPU units live in, taken from the gap between the
/// physmap and the kernel image.
///
/// Each CPU gets `ARCH_CPU_LOCAL_UNIT_STRIDE` of *reserved address space* and
/// only as many backing frames as it currently needs. Because the stride is
/// constant, unit N sits at a fixed distance from unit 0, so one offset per CPU
/// relocates every per-CPU object, and growing the populated part never moves
/// anything that is already handed out.
#define ARCH_CPU_LOCAL_VIRT_BASE   (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE)
#define ARCH_CPU_LOCAL_UNIT_STRIDE MB(2)
#define ARCH_CPU_LOCAL_SIZE        MB(64)

#define KERNEL_LOAD_BASE (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + GB(1))

#define USER_ASPACE_BASE    0xffffff8000000000UL
#define USER_ASPACE_SIZE    0x0000008000000000UL
#define USER_ASPACE_RESTRICTED_SIZE 0

#endif // #ifndef OURS_ARCH_ASPACE_LAYOUT_HPP 