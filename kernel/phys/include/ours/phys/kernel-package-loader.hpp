/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_PACKAGE_LOADER_HPP
#define OURS_PHYS_KERNEL_PACKAGE_LOADER_HPP 1

#include <bootfs/view.hpp>
#include <obi/kernel-package.hpp>
#include <ours/compiler.hpp>
#include <ours/phys/kernel-package-handoff.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

#include <string_view>

namespace ours::phys {
    /// Resolves the kernel ELF from the kernel-package BootFS that physload
    /// already materialized (PhysLoadHandoff::kernel_storage_base/size):
    /// it views the decompressed BootFS directly and locates the kernel
    /// image inside the `kernel.name` directory (the fixed file-name
    /// vocabulary — image.elf / code-patches — is owned by
    /// obi::KernelPackage).
    class KernelPackageLoader final {
      public:
        using Bytes = ustl::views::Span<u8 const>;

        /// `storage` is the materialized kernel package (must stay alive);
        /// `package_base`/`package_size` describe the retained outer OBI
        /// range carried on to the kernel handoff; `kernel_dir` is the
        /// BootFS directory named by `kernel.name` (e.g. "vmours").
        auto init(Bytes storage, PhysAddr package_base, usize package_size,
                  std::string_view kernel_dir) -> Status;

        [[nodiscard]] auto kernel() const -> Bytes {
            return kernel_;
        }

        [[nodiscard]] auto handoff() const -> BootPackageHandoff const & {
            return handoff_;
        }

      private:
        [[nodiscard]] static auto handoff_payload(Bytes payload) -> PackagePayloadHandoff;

        Bytes kernel_;
        BootPackageHandoff handoff_;
    };
} // namespace ours::phys

#endif // OURS_PHYS_KERNEL_PACKAGE_LOADER_HPP
