/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_INTERRUPT_HPP
#define OURS_INTERRUPT_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// interrupt_create's options. The values are ABI and never change.
///
/// Virtual makes an interrupt object with no line behind it, raised by
/// interrupt_trigger. Without it the object is on a physical line, and the
/// mode says how that line signals: the three bits of ModeMask. ModeDefault
/// leaves the line signalling as it does now, as the firmware or its last
/// owner left it; the other four set it. 5 to 7 in the field are refused: 5
/// would be both edges, which no controller here can do.
///
/// The bit 0x1 was RemapIrq, which took the line as an ISA IRQ number. It is
/// retired and reserved, and not given another meaning: a create that sets it
/// is refused (InvArgs). A program learns which pin a legacy device is on
/// from whoever reads the firmware's tables, and asks interrupt_map for the
/// number of that pin's line.
/// A level-triggered line is masked from the moment its interrupt is
/// delivered until it is acknowledged, since the device holds the level
/// until its driver has dealt with it; an edge-triggered line is not.
///
/// A line is named by the kernel's number for it, assigned at boot: never the
/// pin's number in a controller's own numbering, and not the same on another
/// machine or another kernel, so a program does not compute one.
/// interrupt_map resolves the number for a pin from the pair that names it:
/// the controller's interrupt domain and the pin's number in that domain's
/// numbering (a GSI on an ACPI platform), both taken from the route service's
/// answer, to the holder of the interrupt root.
///
/// The root controller's domain is 0 on every platform, and no second domain
/// exists yet; the rule a second domain's ids follow is written here when one
/// is registered.
inline constexpr auto ROOT_INTERRUPT_DOMAIN = u32{0};

/// No option picks the clock a timestamp is taken on or makes the line a
/// wake source: this kernel has one clock, and no suspend to wake from.
enum class InterruptOptions : u32 {
    None = 0,
    ModeDefault = 0,
    ModeEdgeLow = 1U << 1,
    ModeEdgeHigh = 2U << 1,
    ModeLevelLow = 3U << 1,
    ModeLevelHigh = 4U << 1,
    ModeMask = 0xE,
    Virtual = 0x10,
};
USTL_ENABLE_ENUM_BITMASK(InterruptOptions);

/// interrupt_trigger's options. None is the only one; any other value is
/// refused.
enum class InterruptTriggerOptions : u32 {
    None = 0,
};

/// interrupt_bind's options: one of the two, not bits. The values are ABI and
/// never change.
enum class InterruptBindOptions : u32 {
    Bind = 0,
    Unbind = 1,
};
} // namespace ours

#endif // #ifndef OURS_INTERRUPT_HPP
