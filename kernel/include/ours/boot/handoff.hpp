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
/// Panics once the image has been taken (take_boot_image()): its memory is
/// no longer the kernel's to read.
[[nodiscard]]
auto boot_package() -> phys::BootObi::Result;

/// Takes the boot image away from the kernel, for userboot: returns its
/// physical range (whole pages from a page-aligned base, as phys reserved it;
/// `size` is the image's own length), and from then on boot_package() and
/// kernel_package() panic. Once only, by userabi::userboot_init(), after the
/// kernel has read everything it needs from the image.
///
/// Zircon's counterpart is EndHandoff() (zircon/kernel/top/handoff.cc), after
/// which nothing reads gPhysHandoff's ZBI and the ZBI's pages belong to
/// userboot's VMO.
[[nodiscard]]
auto take_boot_image() -> phys::PackagePayloadHandoff;

/// The boot-critical kernel package (the object a `kernel_package` GN
/// template produces, a StorageKernel BootFS), viewed in place: physload
/// decompressed it into memory the kernel never reclaims, and the handoff
/// carries its range (BootPackageHandoff::kernel_storage). Nothing is
/// allocated or decompressed, and the view's files are valid for the life of
/// the kernel. Its envelope() is not: that is the item header inside the boot
/// image, gone with take_boot_image(). Panics once the image is taken.
[[nodiscard]]
auto kernel_package() -> phys::KernelPackage::Result;
} // namespace ours::boot

#endif // OURS_BOOT_HANDOFF_HPP
