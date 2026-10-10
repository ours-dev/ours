/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_OPTIONS_HPP
#define ARCH_PAGING_OPTIONS_HPP 1

#include <arch/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
enum class MapOptions {
    SkipIfExisting,
    OverwriteIfExisting,
    ErrorIfExisting,
    TryLargePage = 2_bit,

    ///
    TryMerge = 3_bit,
};
USTL_ENABLE_ENUM_BITMASK(MapOptions);

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