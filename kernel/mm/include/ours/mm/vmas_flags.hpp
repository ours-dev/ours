/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VMAS_FLAGS_HPP
#define OURS_MM_VMAS_FLAGS_HPP 1

#include <arch/paging/controls.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
using ::arch::paging::HarvestControl;
using ::arch::paging::MapControl;
using ::arch::paging::UnmapControl;

enum class VmasFlags : usize {
    User,
    Kernel,
    Guest,
    OomIgnored = 2_bit,
    AreaMergeable = 3_bit,
    Shared = 4_bit,
    Active = 5_bit,

    /// Set if ASLR enabled.
    Aslr = 31_bit,
};
USTL_ENABLE_ENUM_BITMASK(VmasFlags);

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMAS_FLAGS_HPP
