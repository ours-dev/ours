/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VMM_HPP
#define OURS_MM_VMM_HPP 1

#include <ours/mm/physmap.hpp>
#include <ours/mm/types.hpp>

namespace ours::mm {
/// Do not use the following symbols directly.
extern char const IMAGE_START[] LINK_NAME("__executable_start");
extern char const IMAGE_END[] LINK_NAME("__executable_end");

extern char const KERNEL_CODE_START[] LINK_NAME("__code_start");
extern char const KERNEL_CODE_END[] LINK_NAME("__code_end");

extern char const KERNEL_DATA_START[] LINK_NAME("__data_start");
extern char const KERNEL_DATA_END[] LINK_NAME("__data_end");

extern char const KERNEL_RODATA_START[] LINK_NAME("__rodata_start");
extern char const KERNEL_RODATA_END[] LINK_NAME("__rodata_end");

/// PT_GNU_RELRO: `.got` and `.data.rel.ro`, writable while physboot relocates
/// the image and read-only from the moment it maps it.
extern char const KERNEL_RELRO_START[] LINK_NAME("__relro_start");
extern char const KERNEL_RELRO_END[] LINK_NAME("__relro_end");

extern char const KERNEL_INIT_START[] LINK_NAME("__init_start");
extern char const KERNEL_INIT_END[] LINK_NAME("__init_end");

/// The boot-time data that is only ever read: the init-hook table, the counter
/// descriptors, the scheduler-discipline registry (kernel.ld's `.init.rodata`).
///
/// Both ends are page aligned -- kernel.ld puts an ALIGN(MAXPAGESIZE) on each
/// side -- which is what lets the range be re-protected without splitting a
/// mapping; see init_vmm.cpp.
extern char const KERNEL_INIT_RODATA_START[] LINK_NAME("__init_rodata_start");
extern char const KERNEL_INIT_RODATA_END[] LINK_NAME("__init_rodata_end");

extern char const KERNEL_BSS_START[] LINK_NAME("__bss_start");
extern char const KERNEL_BSS_END[] LINK_NAME("__bss_end");

FORCE_INLINE
constexpr auto get_kernel_size() -> usize {
    return IMAGE_END - IMAGE_START;
}

extern "C" VmPage *ZERO_PAGE;
extern "C" PhysAddr KERNEL_PHYS_BASE;
extern "C" VirtAddr KERNEL_VIRT_BASE;

FORCE_INLINE
constexpr auto get_zero_page() -> VmPage * {
    return ZERO_PAGE;
}

FORCE_INLINE
constexpr auto get_kernel_phys_base() -> PhysAddr {
    return KERNEL_PHYS_BASE;
}

/// Translates a kernel-image virtual address to the physical address behind it.
///
/// PhysMap's translation does not cover these: the image is mapped at the
/// kernel's load base, outside the physmap window, so PhysMap::virt_to_phys()
/// rejects such an address and answers PhysAddr::Invalid. Physboot
/// placed the whole image in one contiguous physical allocation and handed its
/// base over as KERNEL_PHYS_BASE, so the image offset carries straight across.
FORCE_INLINE
inline auto kernel_image_virt_to_phys(VirtAddr virt_addr) -> PhysAddr {
    return KERNEL_PHYS_BASE + (virt_addr - VirtAddr{reinterpret_cast<usize>(IMAGE_START)});
}

FORCE_INLINE [[nodiscard]]
constexpr auto get_kernel_virt_base() -> VirtAddr {
    return KERNEL_VIRT_BASE;
}

/// Defined by arch-specific code. Must be initialized in `init_arch_early`.
extern "C" u8 ARCH_PHYS_ADDR_BITS;
extern "C" u8 ARCH_VIRT_ADDR_BITS;

FORCE_INLINE
constexpr auto get_phys_addr_bits() -> u8 {
    return ARCH_PHYS_ADDR_BITS;
}

FORCE_INLINE
constexpr auto get_virt_addr_bits() -> u8 {
    return ARCH_VIRT_ADDR_BITS;
}

auto is_kernel_address(VirtAddr va) -> bool;

auto virt_to_phys(VirtAddr) -> PhysAddr;

auto phys_to_virt(PhysAddr) -> VirtAddr;

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMM_HPP
