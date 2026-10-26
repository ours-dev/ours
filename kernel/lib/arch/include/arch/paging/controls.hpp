/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_OPTIONS_HPP
#define ARCH_PAGING_OPTIONS_HPP 1

#include <arch/types.hpp>

#include <bit>
#include <utility>

#include <ustl/util/enum_bits.hpp>

namespace arch::paging {
/// What a map does, chosen by its caller in two respects, one bit of each
/// group:
///
///   - where a page of the range is mapped already: ErrorIfExisting fails
///     the map, SkipIfExisting leaves that entry as it is and goes on,
///     OverwriteIfExisting replaces it;
///   - when the map fails part way: UndoOnFailure unmaps everything the call
///     got through, so that a failure leaves nothing of it (with Skip, that
///     takes the entries it skipped there too); KeepOnFailure leaves it as
///     it is and says how far the call got.
///
/// No choice is zero and none is implied, so that a caller cannot get one by
/// leaving it out: a value missing a group, or with two bits of one, is not
/// a map anyone can ask for (map_options_valid()).
///
/// Which leaf sizes a map uses is not an option: the page table chooses the
/// largest each step of the range allows.
enum class MapOptions {
    ErrorIfExisting = 0_bit,
    SkipIfExisting = 1_bit,
    OverwriteIfExisting = 2_bit,
    UndoOnFailure = 3_bit,
    KeepOnFailure = 4_bit,
};
USTL_ENABLE_ENUM_BITMASK(MapOptions);

/// The two groups of MapOptions.
constexpr auto MAP_OPTIONS_EXISTING =
    MapOptions::ErrorIfExisting | MapOptions::SkipIfExisting | MapOptions::OverwriteIfExisting;
constexpr auto MAP_OPTIONS_FAILURE = MapOptions::UndoOnFailure | MapOptions::KeepOnFailure;

/// Exactly one bit of each group, and no bit outside them.
constexpr auto map_options_valid(MapOptions const options) -> bool {
    auto const all = std::to_underlying(options);
    auto const existing = all & std::to_underlying(MAP_OPTIONS_EXISTING);
    auto const failure = all & std::to_underlying(MAP_OPTIONS_FAILURE);
    return std::has_single_bit(static_cast<unsigned>(existing)) &&
           std::has_single_bit(static_cast<unsigned>(failure)) && (existing | failure) == all;
}

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