/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_RIGHTS_HPP
#define OURS_RIGHTS_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// What a handle permits. Rights live on the handle, not on the object: two
/// handles to one channel may carry different rights, and a system call checks
/// the handle it was given.
///
/// The bit values are ABI and never change. All of them are named, including
/// the ones no object here uses yet. This used to be five bits in an order of
/// its own, with Signal on bit 4 where Execute is now; nothing in user space
/// depended on the values, so that was the cheapest moment to lay out the
/// whole set.
enum class Rights : u32 {
    None = 0,
    Duplicate = 0_bit,
    Transfer = 1_bit,
    Read = 2_bit,
    Write = 3_bit,
    Execute = 4_bit,
    Map = 5_bit,
    GetProperty = 6_bit,
    SetProperty = 7_bit,
    Enumerate = 8_bit,
    Destroy = 9_bit,
    SetPolicy = 10_bit,
    GetPolicy = 11_bit,
    Signal = 12_bit,
    SignalPeer = 13_bit,
    Wait = 14_bit,
    Inspect = 15_bit,
    ManageJob = 16_bit,
    ManageProcess = 17_bit,
    ManageThread = 18_bit,
    ApplyProfile = 19_bit,
    ManageSocket = 20_bit,
    OpChildren = 21_bit,
    Resize = 22_bit,
    AttachVmo = 23_bit,
    ManageVmo = 24_bit,

    /// Not a right. Passed to handle_duplicate, as the whole word, to mean
    /// "the rights the source handle already has"; beside any other bit it is
    /// just bit 31, which no handle has (InvArgs).
    SameRights = 31_bit,

    /// The groups the per-object default rights are composed from.
    Basic = Transfer | Duplicate | Wait | Inspect,
    Io = Read | Write,
    Property = GetProperty | SetProperty,
    Policy = GetPolicy | SetPolicy,
};
USTL_ENABLE_ENUM_BITMASK(Rights);

// Per-object default rights are kernel policy rather than part of this
// public ABI header, so each one lives beside the dispatcher it belongs to
// (RIGHTS_DEFAULT_THREAD in thread_dispatcher.hpp, RIGHTS_DEFAULT_VMO in
// vmo_dispatcher.hpp, and so on).
} // namespace ours

#endif // #ifndef OURS_RIGHTS_HPP
