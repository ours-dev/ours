/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_ASPACE_FLAGS_HPP
#define OURS_MEM_VM_ASPACE_FLAGS_HPP 1

#include <ours/status.hpp>
#include <ours/mem/types.hpp>

#include <ustl/result.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/traits/function_detector.hpp>

#include <arch/paging/controls.hpp>

namespace ours::mem {
    using arch::paging::MapControl;
    using arch::paging::UnmapControl;
    using arch::paging::HarvestControl;

    enum class VmasFlags: usize {
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

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_ASPACE_FLAGS_HPP