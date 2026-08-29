/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_BOOT_HPP
#define OURS_PHYS_KERNEL_BOOT_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/boot_options.hpp>
#include <ours/phys/elf_image.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/kernel_package_handoff.hpp>
#include <ours/phys/kernel_package_loader.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/option.hpp>

namespace ours::phys {
class PHYS_EXPORT Aspace;
struct KernelBoot {
    /// `bootmem` is physload's live allocator -- the module has no BootMem
    /// of its own and allocates through this pointer.
    auto init(KernelPackageLoader &kpackage, BootMem *bootmem) -> void;

    auto load() -> void;

    auto boot() -> void;

    FORCE_INLINE constexpr auto image() -> ElfImage & {
        return kimage_;
    }

    FORCE_INLINE constexpr auto load_address() -> VirtAddr {
        return KERNEL_LOAD_BASE + kaslr_offset_;
    }

    FORCE_INLINE constexpr auto kernel_addr() const -> PhysAddr {
        return kernel_addr_;
    }

    FORCE_INLINE constexpr auto kernel_size() const -> usize {
        return kernel_size_;
    }

    FORCE_INLINE constexpr auto kernel_entry_point() const -> usize {
        return kernel_entry_;
    }

  private:
    auto gen_load_bias(usize const limit) -> usize;
    auto build_kernel_aspace() -> void;
    auto do_handoff() -> void;
    auto arch_do_handoff() -> void;

    /// Top-down allocate()-or-panic through bootmem_, spanning its full
    /// address range.  `what` names the allocation in the panic message.
    auto alloc_top_down(usize size, usize alignment, char const *what) -> PhysAddr;

    template <typename... Args>
    auto invoke_kernel(Args... args) -> void {
        (*reinterpret_cast<auto (*)(Args...)->void>(kernel_entry_))(args...);
    }

    PhysAddr kernel_addr_;
    PhysAddr kernel_size_;
    VirtAddr kernel_entry_;
    usize kaslr_offset_;
    BootMem *bootmem_;
    BootPackageHandoff package_handoff_;
    /// The kernel payload, always an ELF: the flat kernel-package model puts
    /// image.elf inside the StorageKernel BootFS, and phys_executable()'s
    /// flat `.bin` output is what a *bootloader* consumes, never something
    /// this loads.
    ElfImage kimage_;
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_KERNEL_BOOT_HPP
