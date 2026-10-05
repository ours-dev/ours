/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_INTERRUPT_HPP
#define OURS_INTERRUPT_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// interrupt_create's options. The values are Zircon's ZX_INTERRUPT_*
/// (zircon/types.h), so that code ported from Zircon asks for the same thing.
///
/// Virtual is the only one accepted so far: an interrupt object with no line
/// behind it, raised by interrupt_trigger. Zircon's other options name how a
/// physical line is reached and triggered -- REMAP_IRQ (bit 0), the trigger
/// mode in bits 1-3 -- and arrive with physical lines, under the same
/// values; until then a create without Virtual answers NotSupported.
/// Zircon's TIMESTAMP_MONO and WAKE_VECTOR have no counterpart: this kernel
/// has one clock, and no suspend to wake from.
enum class InterruptOptions : u32 {
    None = 0,
    Virtual = 0x10,
};
USTL_ENABLE_ENUM_BITMASK(InterruptOptions);

/// interrupt_trigger's options. None is the only one, as in Zircon, whose
/// zx_interrupt_trigger() refuses any non-zero options.
enum class InterruptTriggerOptions : u32 {
    None = 0,
};
} // namespace ours

#endif // #ifndef OURS_INTERRUPT_HPP
