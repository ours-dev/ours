/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_INTERRUPT_HPP
#define OURS_INTERRUPT_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// interrupt_create's options. The values are ABI and never change.
///
/// Virtual is the only one accepted so far: an interrupt object with no line
/// behind it, raised by interrupt_trigger. The options that name how a
/// physical line is reached and triggered -- a remapped line (bit 0), the
/// trigger mode in bits 1-3 -- arrive with physical lines, at those bits;
/// until then a create without Virtual answers NotSupported. No option picks
/// the clock a timestamp is taken on or makes the line a wake source: this
/// kernel has one clock, and no suspend to wake from.
enum class InterruptOptions : u32 {
    None = 0,
    Virtual = 0x10,
};
USTL_ENABLE_ENUM_BITMASK(InterruptOptions);

/// interrupt_trigger's options. None is the only one; any other value is
/// refused.
enum class InterruptTriggerOptions : u32 {
    None = 0,
};
} // namespace ours

#endif // #ifndef OURS_INTERRUPT_HPP
