/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_SIGNALS_HPP
#define OURS_SIGNALS_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
enum class Signals : u32 {
    None,
    Suspend = 0_bit,
    Kill = 1_bit,

    MaxCount,
};
USTL_ENABLE_ENUM_BITMASK(Signals);
} // namespace ours

#endif // #ifndef OURS_RIGHTS_HPP
