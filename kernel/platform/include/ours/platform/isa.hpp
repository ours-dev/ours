/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_ISA_HPP
#define OURS_PLATFORM_ISA_HPP 1

#include <ours/types.hpp>

namespace ours {
/// The fixed IRQ line assignments of the legacy PC/AT interrupt controllers.
///
/// These are *ISA* numbers, the ones the ACPI interrupt source overrides are
/// written in terms of; apic_io_isa_to_global() maps one to its global system
/// interrupt, and the two are frequently different (IRQ 0 is commonly
/// delivered on GSI 2). Past the platform the GSI is the pin: it indexes the
/// IO-APIC's redirection table and picks the vector. The irq layer names the
/// line made for a pin by its own number, its virq, which is what
/// irq::attach() takes: platform::isa_irq_line() below turns an ISA number
/// into it, so convert with that before using any of these there.
///
/// Constants rather than the `#define`s these used to be: a macro has no type
/// and no namespace, so it took part in whatever arithmetic it landed in and
/// claimed its name in every translation unit that included this header.
/// Typed as IsaIrqNum, they only convert where a conversion makes sense.
using IsaIrqNum = u8;

constexpr auto ISA_IRQ_PIT = IsaIrqNum(0);
constexpr auto ISA_IRQ_KEYBOARD = IsaIrqNum(1);
constexpr auto ISA_IRQ_PIC2 = IsaIrqNum(2);
constexpr auto ISA_IRQ_SERIAL2 = IsaIrqNum(3);
constexpr auto ISA_IRQ_SERIAL1 = IsaIrqNum(4);
constexpr auto ISA_IRQ_PRINTER1 = IsaIrqNum(7);
constexpr auto ISA_IRQ_CMOSRTC = IsaIrqNum(8);
constexpr auto ISA_IRQ_PS2MOUSE = IsaIrqNum(12);
constexpr auto ISA_IRQ_IDE0 = IsaIrqNum(14);
constexpr auto ISA_IRQ_IDE1 = IsaIrqNum(15);

namespace platform {
    /// The interrupt line ISA IRQ `isa_irq` arrives on, by the number the irq
    /// layer names lines with, its virq: on a PC the line made for the pin the
    /// MADT's overrides send it to (q35 sends ISA 0 to GSI 2).
    /// VIrqNum::Invalid if `isa_irq` is no ISA IRQ of this platform (past 15,
    /// or any number on a platform that has no ISA interrupts), if it is the
    /// cascade between the two legacy controllers, ISA 2, which carries no
    /// device, or if no line was made for its pin.
    ///
    /// For the kernel's own code (the timer's lines, the tests): no system
    /// call takes an ISA number, since a program names a pin by the
    /// platform's number for it, which interrupt_map resolves (from the
    /// domain and source the route service's answer names).
    [[nodiscard]]
    auto isa_irq_line(u32 isa_irq) -> VIrqNum;
} // namespace platform
} // namespace ours

#endif // #ifndef OURS_PLATFORM_ISA_HPP
