/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_PACKAGE_HANDOFF_HPP
#define OURS_PHYS_KERNEL_PACKAGE_HANDOFF_HPP 1

#include <ours/types.hpp>

#include <limits>

namespace ours::phys {
/// A retained physical payload.  `present` distinguishes an absent
/// item from an explicitly present zero-length payload.
struct PackagePayloadHandoff {
    PhysAddr base = 0;
    usize size = 0;
    bool present = false;
    u8 reserved[sizeof(usize) - 1] = {};

    [[nodiscard]]
    auto valid() const -> bool {
        if (!reserved_is_zero()) {
            return false;
        }
        if (!present) {
            return base == 0 && size == 0;
        }
        return base != 0 && size <= std::numeric_limits<PhysAddr>::max() - base;
    }

    [[nodiscard]]
    auto fits_within(PhysAddr const range_base, usize const range_size) const -> bool {
        if (!valid()) {
            return false;
        }
        if (range_size > std::numeric_limits<PhysAddr>::max() - range_base) {
            return false;
        }
        if (!present) {
            return true;
        }
        if (base < range_base) {
            return false;
        }
        auto const offset = base - range_base;
        return offset <= range_size && size <= range_size - offset;
    }

  private:
    [[nodiscard]]
    auto reserved_is_zero() const -> bool {
        for (auto const byte: reserved) {
            if (byte != 0) {
                return false;
            }
        }
        return true;
    }
};
static_assert(sizeof(PackagePayloadHandoff) == 3 * sizeof(usize));

/// What the kernel gets of the boot image: two retained ranges.
struct BootPackageHandoff {
    /// The outer OBI container, where the bootloader put it, so later stages
    /// can re-parse it (ours::boot::boot_package()).
    PackagePayloadHandoff package;

    /// The kernel package (the StorageKernel BootFS) as physload decompressed
    /// it (ModuleLoader::init()), in bootmem it never releases: PhysImage,
    /// which the kernel's allocator does not take back. The kernel reads
    /// userboot and the vDSO out of it (ours::boot::kernel_package()) rather
    /// than decompressing the package a second time into memory of its own.
    PackagePayloadHandoff kernel_storage;

    [[nodiscard]]
    auto valid() const -> bool {
        return package.present && package.size != 0 && package.valid() && kernel_storage.valid();
    }

    [[nodiscard]]
    auto fits_within(PhysAddr const range_base, usize const range_size) const -> bool {
        return valid() && package.fits_within(range_base, range_size) &&
               kernel_storage.fits_within(range_base, range_size);
    }
};
} // namespace ours::phys

#endif // OURS_PHYS_KERNEL_PACKAGE_HANDOFF_HPP
