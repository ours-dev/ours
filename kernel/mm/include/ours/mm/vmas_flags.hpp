/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VMAS_FLAGS_HPP
#define OURS_MM_VMAS_FLAGS_HPP 1

#include <ours/mm/types.hpp>
#include <ours/status.hpp>

#include <arch/paging/controls.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
using ::arch::paging::HarvestControl;
using ::arch::paging::MapControl;
using ::arch::paging::UnmapControl;

/// Which address space this is.
///
/// An enum and not a bit in VmasFlags, because it is a choice of exactly one
/// of three, and a flag word cannot say that: `User | Kernel` would be a
/// representable value with no meaning, and every reader would have to invent
/// its own rule for what to do with it.
///
/// It used to be three flag bits, and the damage was not theoretical. Every
/// test asked with a bitwise and, `User` was the enumerator with no value and
/// therefore 0, and `x & 0` is 0 -- so the User test was false for every
/// address space ever created, and VmAspace::create() fell through to its
/// kernel branch each time. Every "user" address space in this kernel spanned
/// KERNEL_ASPACE_BASE for 512 GiB. Nothing noticed until something first
/// tried to map into one.
///
/// Giving the three distinct bits would have fixed that one instance and left
/// the shape that produced it. As an enum, each kind has one value, a switch
/// over it is checked for exhaustiveness, and there is no catch-all branch
/// for a fourth kind to fall into silently.
enum class VmasKind {
    User,
    Kernel,
    Guest,
};

/// Properties an address space carries in any combination. Unlike the kind
/// above, these really are independent bits.
enum class VmasFlags : usize {
    None = 0,

    OomIgnored = 0_bit,
    AreaMergeable = 1_bit,
    Shared = 2_bit,
    Active = 3_bit,

    /// Set if ASLR enabled.
    Aslr = 31_bit,
};
USTL_ENABLE_ENUM_BITMASK(VmasFlags);

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMAS_FLAGS_HPP
