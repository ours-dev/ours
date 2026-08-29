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

extern char const KERNEL_INIT_START[] LINK_NAME("__init_start");
extern char const KERNEL_INIT_END[] LINK_NAME("__init_end");

extern char const KERNEL_BSS_START[] LINK_NAME("__bss_start");
extern char const KERNEL_BSS_END[] LINK_NAME("__bss_end");

FORCE_INLINE constexpr auto get_kernel_size() -> usize {
    return IMAGE_END - IMAGE_START;
}

NO_MANGLE VmPage *g_zero_page;
NO_MANGLE PhysAddr g_kernel_phys_base;
NO_MANGLE VirtAddr g_kernel_virt_base;

FORCE_INLINE constexpr auto get_zero_page() -> VmPage * {
    return g_zero_page;
}

FORCE_INLINE constexpr auto get_kernel_phys_base() -> PhysAddr {
    return g_kernel_phys_base;
}

FORCE_INLINE [[nodiscard]]
constexpr auto get_kernel_virt_base() -> VirtAddr {
    return g_kernel_virt_base;
}

/// Defined by arch-specific code. Must be initialized in `init_arch_early`.
NO_MANGLE u8 g_arch_phys_addr_bits;
NO_MANGLE u8 g_arch_virt_addr_bits;

FORCE_INLINE constexpr auto get_phys_addr_bits() -> PhysAddr {
    return g_arch_phys_addr_bits;
}

FORCE_INLINE constexpr auto get_virt_addr_bits() -> VirtAddr {
    return g_arch_virt_addr_bits;
}

auto is_kernel_address(VirtAddr va) -> bool;

auto virt_to_phys(VirtAddr) -> PhysAddr;

auto phys_to_virt(PhysAddr) -> VirtAddr;

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMM_HPP
