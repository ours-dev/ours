/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ASPACE_LAYOUT_HPP
#define OURS_ARCH_ASPACE_LAYOUT_HPP 1

#include <ours/macro_abi.hpp>

#define KERNEL_ASPACE_BASE 0xffffff8000000000UL
#define KERNEL_ASPACE_SIZE GB(512)

#define ARCH_PHYSMAP_PHYS_BASE 0
#define ARCH_PHYSMAP_VIRT_BASE KERNEL_ASPACE_BASE
#define ARCH_PHYSMAP_SIZE GB(32)

/// Virtual window per-CPU chunks may be placed in, taken from the gap between
/// the physmap and the kernel image.
///
/// The window is carved into slots of `ARCH_CPU_LOCAL_CHUNK_SLOT`, one per
/// chunk. A slot has to hold `MAX_CPU * unit_size`. Placing a chunk here rather
/// than in one contiguous physical block is optional -- it avoids needing a
/// large contiguous allocation, and lets each unit be backed from its own NUMA
/// node -- so an architecture with no address space to spare simply reports
/// `ArchCpuLocal::HAS_RESERVED_WINDOW == false` and does without.
#define ARCH_CPU_LOCAL_VIRT_BASE (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE)
#define ARCH_CPU_LOCAL_CHUNK_SLOT MB(2)
#define ARCH_CPU_LOCAL_SIZE MB(64)

#define KERNEL_LOAD_BASE (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + GB(1))

#define USER_ASPACE_BASE 0xffffff8000000000UL
#define USER_ASPACE_SIZE 0x0000008000000000UL
#define USER_ASPACE_RESTRICTED_SIZE 0

#endif // #ifndef OURS_ARCH_ASPACE_LAYOUT_HPP