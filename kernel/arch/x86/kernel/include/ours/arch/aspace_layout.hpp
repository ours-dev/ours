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

/// The window mm::vmalloc() carves virtually-contiguous kernel allocations
/// from.  Backed page by page from wherever the buddy allocator has room, so
/// unlike the physmap there is no relationship at all between an address in
/// here and a physical one -- PhysMap::virt_to_phys() answers zero for it.
///
/// Above the kernel image rather than in the gap below it, and that is a
/// constraint rather than a preference: the gap between the per-CPU window
/// and KERNEL_LOAD_BASE is under a gigabyte, and the image's own base moves
/// with up to GB(1) of KASLR bias on top of KERNEL_LOAD_BASE. A window placed
/// below would have to be both small and dependent on where the image landed.
/// GB(64) clears the bias by a wide margin while leaving that gap to the
/// lowest-address first-fit allocations that already live there (the local
/// APIC, the HPET, the IDT and GDT, every kernel stack).
///
/// Being a *fixed* range is what makes mm::is_vmalloc_addr() a subtraction
/// rather than a tree walk, and that in turn is what lets `operator delete`
/// tell a vmalloc pointer from a kmalloc one -- ktl::kfree() resolves its
/// pointer through the physmap and cannot be asked about an address outside
/// it. Linux's VMALLOC_START/VMALLOC_END are the same idea.
#define ARCH_VMALLOC_VIRT_BASE (KERNEL_ASPACE_BASE + GB(64))
#define ARCH_VMALLOC_SIZE GB(4)

/// The user half of the canonical address space.
///
/// x86-64 splits the canonical range in two: bits 63..47 must all copy bit 47,
/// so addresses are either 0x0000'0000'0000'0000-0x0000'7fff'ffff'ffff (user)
/// or 0xffff'8000'0000'0000-0xffff'ffff'ffff'ffff (kernel). A user mapping
/// therefore *cannot* live above the split -- ring 3 loading such an address
/// faults regardless of what the page tables say.
///
/// This used to read `0xffff'ff80'0000'0000UL`, byte for byte the same as
/// KERNEL_ASPACE_BASE below, i.e. the physmap. Nothing had entered user mode
/// yet, so nothing had noticed.
///
/// The base is 2 MiB rather than 0 so that a null pointer, and anything
/// reached by a small offset from one, lands in a hole that can never be
/// mapped -- the same reason and the same number as Zircon's
/// USER_ASPACE_BASE. The size stops one page below the 2^47 split: that last
/// page is left out because an address one past the end of a mapping at the
/// very top would be non-canonical, and bounds arithmetic is easier when the
/// end is still a representable address.
///
/// That missing page is also what makes SYSRET safe, and must not be given
/// back. SYSRET returns to the address after the SYSCALL, and a SYSCALL in the
/// last two bytes of a page at 0x7fff'ffff'f000 would return to
/// 0x8000'0000'0000 -- non-canonical, which on Intel faults in ring 0 on the
/// user's stack and %gs (see arch_is_valid_user_pc()). With the page unmappable
/// no instruction can end there, so syscall.S returns without checking. It is
/// the first of the measures in Zircon's docs/concepts/kernel/
/// sysret_problem.md, and Zircon leaves the same page out.
#define USER_ASPACE_BASE MB(2)
#define USER_ASPACE_SIZE ((1UL << 47) - PAGE_SIZE - USER_ASPACE_BASE)

/// The "restricted" sub-range, for address spaces that must stay within a
/// smaller window than the full user half. Zircon uses half of it
/// (`(1 << 46) - USER_ASPACE_BASE`) for its restricted-mode aspaces.
///
/// It was 0, which made ProcessDispatcher's PRIVATE_ASPACE_SIZE zero too --
/// a private range of no bytes at all.
#define USER_ASPACE_RESTRICTED_SIZE ((1UL << 46) - USER_ASPACE_BASE)

#endif // #ifndef OURS_ARCH_ASPACE_LAYOUT_HPP