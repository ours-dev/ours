/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_RIGHTS_HPP
#define OURS_RIGHTS_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
    enum class Rights: u32 {
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

    constexpr auto RIGHTS_DEFAULT_THREAD = Rights::Signal | Rights::Io;
    constexpr auto RIGHTS_DEFAULT_PROCESS = Rights::Signal | Rights::Io;
    constexpr auto RIGHTS_DEFAULT_VMO = Rights::Signal | Rights::Io;
    constexpr auto RIGHTS_DEFAULT_VMAR = Rights::Signal | Rights::Io;
} // namespace ours

#endif // #ifndef OURS_RIGHTS_HPP
