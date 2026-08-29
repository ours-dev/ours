/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BOOT_HANDOFF_HPP
#define OURS_BOOT_HANDOFF_HPP 1

#include <ours/phys/boot_obi.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/types.hpp>

#include <ustl/views/span.hpp>

namespace ours::boot {
    /// Validates and installs the phys-to-kernel handoff during kernel entry.
    auto setup_handoff(PhysAddr handoff) -> void;

    /// The validated handoff remains available for later boot stages, such as
    /// userboot, through the kernel physmap.
    [[nodiscard]] auto handoff() -> phys::Handoff const &;

    /// Reconstruct the validated top-level OBI package (the object a
    /// `kernel_image` GN template produces) from the retained handoff range.
    [[nodiscard]] auto boot_package() -> phys::BootObi::Result;

    /// Reconstruct the boot-critical kernel package (the object a
    /// `kernel_package` GN template produces, a StorageKernel BootFS).
    /// `buffer` receives the decompressed BootFS and must be large enough
    /// (>= the StorageKernel item's `memory_size`) and outlive the returned
    /// view.
    [[nodiscard]] auto kernel_package(ustl::views::Span<u8> buffer)
        -> phys::KernelPackage::Result;
} // namespace ours::boot

#endif // OURS_BOOT_HANDOFF_HPP
