/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ASPACE_LAYOUT_HPP
#define OURS_ARCH_ASPACE_LAYOUT_HPP 1

/// The riscv virtual address space under Sv39, where the kernel runs.
///
/// These are macros, not constexpr values, for the same two readers as on
/// every other architecture: assembly may include this file, and
/// kernel/mm/cpu_local.cpp asks `#ifndef ARCH_CPU_LOCAL_VIRT_BASE` whether an
/// architecture has a CPU-local window at all.
///
/// Sv39 translates 39-bit addresses, and bits 63..39 of every address must
/// copy bit 38. That leaves two 256 GiB halves: [0, 1 << 38) at the bottom,
/// for user space, and the top 256 GiB, for the kernel. The root table has
/// 512 entries of 1 GiB each; the kernel half is its upper 256.
///
///   0xffff'ffc0'0000'0000  KERNEL_ASPACE_BASE: physmap, 64 GiB
///   0xffff'ffd0'0000'0000  CPU-local window, 64 MiB
///   0xffff'ffd0'4000'0000  KERNEL_LOAD_BASE: the kernel image, plus up to
///                          1 GiB of KASLR bias
///   0xffff'ffe0'0000'0000  vmalloc, 4 GiB
///   0xffff'ffff'ffff'ffff  last address of the kernel half
#define KERNEL_ASPACE_BASE 0xffffffc000000000UL
#define KERNEL_ASPACE_SIZE (1UL << 38) // 256 GiB

/// Every physical address below ARCH_PHYSMAP_SIZE, at the bottom of the kernel
/// half. RAM on QEMU's `virt` machine starts at 2 GiB, so the window holds up
/// to 62 GiB of it, and everything below RAM (the platform's devices) is in it
/// too: the physmap is a window onto physical addresses, not onto RAM.
#define ARCH_PHYSMAP_PHYS_BASE 0
#define ARCH_PHYSMAP_VIRT_BASE KERNEL_ASPACE_BASE
#define ARCH_PHYSMAP_SIZE 0x1000000000UL // 64 GiB

/// Virtual window per-CPU chunks may be placed in, taken from the gap between
/// the physmap and the kernel image.
///
/// The window is carved into slots of `ARCH_CPU_LOCAL_CHUNK_SLOT`, one per
/// chunk, and a slot has to hold `MAX_CPU * unit_size`. Placing a chunk here
/// rather than in one contiguous physical block avoids needing a large
/// contiguous allocation, and lets each unit be backed from its own NUMA node.
#define ARCH_CPU_LOCAL_VIRT_BASE (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE)
#define ARCH_CPU_LOCAL_CHUNK_SLOT 0x200000UL // 2 MiB
#define ARCH_CPU_LOCAL_SIZE 0x4000000UL      // 64 MiB

/// Where physboot places the kernel image, before its KASLR bias of up to
/// 1 GiB. The gap between the CPU-local window and this address is left to the
/// kernel's own first-fit allocations, as is everything between the image and
/// the vmalloc window.
#define KERNEL_LOAD_BASE (KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + 0x40000000UL) // 1 GiB

/// The window mm::vmalloc() carves virtually-contiguous kernel allocations
/// from, backed page by page from wherever the buddy allocator has room, so
/// an address in here says nothing about a physical one.
///
/// Fixed, so that mm::is_vmalloc_addr() is a subtraction and `operator delete`
/// can tell a vmalloc pointer from a kmalloc one. Above the kernel image, so
/// that where KASLR put the image never moves it: halfway up the kernel half
/// is 62 GiB clear of the highest place the image can start.
#define ARCH_VMALLOC_VIRT_BASE (KERNEL_ASPACE_BASE + 0x2000000000UL) // 128 GiB
#define ARCH_VMALLOC_SIZE 0x100000000UL                              // 4 GiB

/// The user half: all of the bottom 256 GiB but its first 2 MiB.
///
/// The base is 2 MiB rather than 0 so that a null pointer, and anything
/// reached by a small offset from one, lands in a hole that can never be
/// mapped.
///
/// The end is the very top of the half, with no page held back. Returning to
/// user mode is an `sret` to whatever address the kernel left in `sepc`, and
/// `sret` drops to user mode before it fetches there, so a return to the first
/// address past the half faults in user mode, as the user program's own fault.
/// An x86-64 SYSRET to the same place would fault in the kernel instead; that
/// is why the x86-64 layout gives its top user page up and this one does not.
#define USER_ASPACE_BASE 0x200000UL // 2 MiB
#define USER_ASPACE_SIZE ((1UL << 38) - USER_ASPACE_BASE)

/// The "restricted" sub-range, for address spaces that must stay within a
/// smaller window than the full user half: up to halfway through the user
/// half, which is the first 128 entries of the root table.
#define USER_ASPACE_RESTRICTED_SIZE ((1UL << 37) - USER_ASPACE_BASE)

#ifndef __ASSEMBLER__
// Sums below are kept as offsets from KERNEL_ASPACE_BASE: the half ends at
// 2^64, so its end as an address wraps to zero.
static_assert(KERNEL_ASPACE_BASE + (KERNEL_ASPACE_SIZE - 1) == ~0UL,
              "The kernel half is the top 256 GiB of the address space");
static_assert((KERNEL_ASPACE_BASE >> 38) == (~0UL >> 38),
              "KERNEL_ASPACE_BASE is the lowest Sv39 address with bit 38 set");

// physboot maps the physmap at KERNEL_ASPACE_BASE, from physical address 0.
static_assert(ARCH_PHYSMAP_VIRT_BASE == KERNEL_ASPACE_BASE && ARCH_PHYSMAP_PHYS_BASE == 0,
              "The physmap is the first thing in the kernel half and starts at physical 0");
static_assert(ARCH_PHYSMAP_SIZE % (1UL << 30) == 0,
              "The physmap is whole 1 GiB leaves, one root-table entry each");
static_assert(ARCH_PHYSMAP_SIZE > 0x80000000UL,
              "The physmap reaches the RAM of QEMU's virt machine, which starts at 2 GiB");

static_assert(ARCH_CPU_LOCAL_VIRT_BASE - KERNEL_ASPACE_BASE >= ARCH_PHYSMAP_SIZE,
              "The CPU-local window starts above the physmap");
static_assert(ARCH_CPU_LOCAL_SIZE % ARCH_CPU_LOCAL_CHUNK_SLOT == 0,
              "The CPU-local window is a whole number of slots");
static_assert(ARCH_CPU_LOCAL_VIRT_BASE % ARCH_CPU_LOCAL_CHUNK_SLOT == 0,
              "Every CPU-local slot is aligned to its size");
static_assert(ARCH_CPU_LOCAL_VIRT_BASE + ARCH_CPU_LOCAL_SIZE <= KERNEL_LOAD_BASE,
              "The CPU-local window ends below the kernel image");

// The image runs from KERNEL_LOAD_BASE plus a bias below 1 GiB, and is given
// at most another 1 GiB for itself.
static_assert(KERNEL_LOAD_BASE % 0x200000UL == 0, "The kernel image starts 2 MiB aligned");
static_assert(KERNEL_LOAD_BASE + (2UL << 30) <= ARCH_VMALLOC_VIRT_BASE,
              "The kernel image, at its highest bias, ends below the vmalloc window");

static_assert(ARCH_VMALLOC_VIRT_BASE % 0x200000UL == 0, "The vmalloc window starts 2 MiB aligned");
static_assert(ARCH_VMALLOC_VIRT_BASE - KERNEL_ASPACE_BASE + ARCH_VMALLOC_SIZE <= KERNEL_ASPACE_SIZE,
              "The vmalloc window ends inside the kernel half");

static_assert(USER_ASPACE_BASE + USER_ASPACE_SIZE == (1UL << 38),
              "The user half ends where Sv39's bottom half does");
static_assert(USER_ASPACE_RESTRICTED_SIZE < USER_ASPACE_SIZE,
              "The restricted range is part of the user half");
#endif // #ifndef __ASSEMBLER__

#endif // #ifndef OURS_ARCH_ASPACE_LAYOUT_HPP
