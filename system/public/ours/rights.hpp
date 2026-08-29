/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_RIGHTS_HPP
#define OURS_RIGHTS_HPP 1

#include <ours/types.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours {
enum class Rights : u32 {
    None,
    Duplicate = 0_bit,
    Transfer = 1_bit,
    Read = 2_bit,
    Write = 3_bit,
    Signal = 4_bit,

    Io = Read | Write,

    Default = Duplicate,
};
USTL_ENABLE_ENUM_BITMASK(Rights);

// Per-object default rights are kernel policy rather than part of this
// public ABI header, so each one lives beside the dispatcher it belongs to
// (RIGHTS_DEFAULT_THREAD in thread-dispatcher.hpp, and so on).
//
// TODO(SmallHuaZi): RIGHTS_DEFAULT_VMO has no home yet -- there is no VM
// object dispatcher header to move it next to. It was `Signal | Io`.
} // namespace ours

#endif // #ifndef OURS_RIGHTS_HPP
