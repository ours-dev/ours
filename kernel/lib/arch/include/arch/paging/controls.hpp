/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_OPTIONS_HPP
#define ARCH_PAGING_OPTIONS_HPP 1

#include <arch/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
/// What a map does where a page of its range is already mapped. One of the
/// three, not bits. Error comes first, so that a value nobody chose is the
/// one that changes nothing it did not mean to.
///
/// Which leaf sizes a map uses is not an option: the page table chooses the
/// largest each step of the range allows.
enum class MapOptions {
    ErrorIfExisting,
    SkipIfExisting,
    OverwriteIfExisting,
};

enum class UnmapOptions {
    /// For semantics.
    None,
};
USTL_ENABLE_ENUM_BITMASK(UnmapOptions);

enum class HarvestOptions {
    /// Only updates the frame's age if it is accessed.
    UpdateAge = 0_bit,

    /// If the entry is unaccessed then we zap it.
    ZapUnaccessed = 1_bit,

    /// If the frame is accessed we remove the accessed flag.
    ResetUnaccessed = 2_bit,
};
USTL_ENABLE_ENUM_BITMASK(HarvestOptions);

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_OPTIONS_HPP