/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_BTI_HPP
#define OURS_BTI_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// iommu_create's |type|: which IOMMU the object stands for. The values are
/// ABI and never change.
///
/// Stub is a machine without an IOMMU, or one the kernel does not drive: it
/// translates nothing and isolates nothing, so an address a device is given
/// is the physical address of the memory, and what the object adds is the
/// bookkeeping of what has been pinned for DMA (BTI, PMT). A driver for a real
/// IOMMU would be another type.
enum class IommuType : u32 {
    Stub = 0,
};

/// bti_pin's options. The values are ABI and never change.
///
/// The Perm bits say what the device may do to the pinned memory, and at least
/// one is needed (InvArgs). Each asks for the matching right on the VMO's
/// handle: Read for PermRead and PermExecute, Write for PermWrite. They are
/// the device's permissions, not the CPU's, which is why PermExecute needs
/// Read and not Execute.
///
/// Compress and Contiguous say how the addresses come back: one per page
/// without either, one per contiguous run of at least the BTI's minimum
/// contiguity with Compress, a single one with Contiguous, which is refused
/// for memory that is not one run. Asking for both is InvArgs.
enum class BtiPinOptions : u32 {
    None = 0,
    PermRead = 1U << 0,
    PermWrite = 1U << 1,
    PermExecute = 1U << 2,
    Compress = 1U << 3,
    Contiguous = 1U << 4,
};
USTL_ENABLE_ENUM_BITMASK(BtiPinOptions);
} // namespace ours

#endif // #ifndef OURS_BTI_HPP
