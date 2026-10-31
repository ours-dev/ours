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
/// other options say how that line is named and how it signals:
///   - RemapIrq: the line is given by its ISA IRQ number, 0 to 15, and the
///     kernel turns it into the line that IRQ arrives on. The firmware's
///     tables may move an ISA IRQ to another pin (QEMU's q35 sends ISA 0 to
///     GSI 2), and the line is named by the kernel's own number for that
///     pin. A number past the ISA IRQs already names a line and is taken as
///     it is.
///   - The mode, the three bits of ModeMask: ModeDefault leaves the line
///     signalling as it does now, as the firmware or its last owner left it;
///     the other four set it. 5 to 7 in the field are refused: 5 would be
///     both edges, which no controller here can do.
/// A level-triggered line is masked from the moment its interrupt is
/// delivered until it is acknowledged, since the device holds the level
/// until its driver has dealt with it; an edge-triggered line is not.
///
/// A line is named by the kernel's number for it, assigned at boot: never the
/// pin's GSI, and not the same on another machine or another kernel, so a
/// program does not compute one. RemapIrq gives the number of an ISA device's
/// line; there is no other way yet to learn the number of another pin's.
///
/// No option picks the clock a timestamp is taken on or makes the line a
/// wake source: this kernel has one clock, and no suspend to wake from.
enum class InterruptOptions : u32 {
    None = 0,
    ModeDefault = 0,
    RemapIrq = 0x1,
    ModeEdgeLow = 1U << 1,
    ModeEdgeHigh = 2U << 1,
    ModeLevelLow = 3U << 1,
    ModeLevelHigh = 4U << 1,
    ModeMask = 0xE,
    Virtual = 0x10,
};
USTL_ENABLE_ENUM_BITMASK(InterruptOptions);

/// What the number given to interrupt_map is the number of. The values are
/// ABI and never change; 0 is no kind, so an argument left zero is refused.
enum class InterruptSourceKind : u32 {
    /// An ISA IRQ, 0 to 15, on a platform that has them (a PC): the number a
    /// legacy device is known by, routed to a pin as the firmware's tables
    /// say. ISA 2, the cascade between the two legacy controllers, carries no
    /// device and names no line.
    Isa = 1,
    /// A global system interrupt: the number the firmware gives a pin of an
    /// interrupt controller (ACPI's GSI).
    Gsi = 2,
};

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
