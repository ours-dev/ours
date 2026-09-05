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
    /// Which half of the address space this is. One bit each, and not the
    /// bare 0/1/2 they used to be: every test in the tree asks with `&`
    /// (`bool(flags & VmasFlags::User)`, `!!(VmasFlags::Kernel & m_flags)`),
    /// and `x & 0` is 0 for every x -- so the User test was false for every
    /// address space ever created. VmAspace::create() therefore took its
    /// kernel branch each time and built user address spaces spanning
    /// KERNEL_ASPACE_BASE, which nothing noticed until something tried to
    /// enter ring 3 in one. Kernel and Guest happened to work at 1 and 2
    /// because those are also single bits; the scheme was one enumerator
    /// away from aliasing.
    User = 0_bit,
    Kernel = 1_bit,
    Guest = 2_bit,

    OomIgnored = 3_bit,
    AreaMergeable = 4_bit,
    Shared = 5_bit,
    Active = 6_bit,

    /// Set if ASLR enabled.
    Aslr = 31_bit,
};
USTL_ENABLE_ENUM_BITMASK(VmasFlags);

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMAS_FLAGS_HPP
