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
[[nodiscard]]
auto handoff() -> phys::Handoff const &;

/// Reconstruct the validated top-level OBI package (the object a
/// `kernel_image` GN template produces) from the retained handoff range.
[[nodiscard]]
auto boot_package() -> phys::BootObi::Result;

/// The boot-critical kernel package (the object a `kernel_package` GN
/// template produces, a StorageKernel BootFS), viewed in place: physload
/// decompressed it into memory the kernel never reclaims, and the handoff
/// carries its range (BootPackageHandoff::kernel_storage). Nothing is
/// allocated or decompressed, and the view is valid for the life of the
/// kernel.
[[nodiscard]]
auto kernel_package() -> phys::KernelPackage::Result;
} // namespace ours::boot

#endif // OURS_BOOT_HANDOFF_HPP
