/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_BOOT_HPP
#define OURS_PHYS_KERNEL_BOOT_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/phys/kernel-package-handoff.hpp>
#include <ours/phys/kernel-package-loader.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/option.hpp>

namespace ours::phys {
class Aspace;
struct KernelBoot {
    /// `early` is the type-erased allocator received from physload — the
    /// module has no BootMem of its own and allocates through it.
    auto init(KernelPackageLoader &kpackage, EarlyMem const &early) -> void;

    auto load() -> void;

    auto boot() -> void;

    FORCE_INLINE constexpr auto image() -> KernelImage & {
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

    template <typename... Args>
    auto invoke_kernel(Args... args) -> void {
        (*reinterpret_cast<auto (*)(Args...)->void>(kernel_entry_))(args...);
    }

    PhysAddr kernel_addr_;
    PhysAddr kernel_size_;
    VirtAddr kernel_entry_;
    usize kaslr_offset_;
    EarlyMem early_;
    BootPackageHandoff package_handoff_;
    KernelImage kimage_;
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_KERNEL_BOOT_HPP
