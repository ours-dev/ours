/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The fake kernel's console and halt on x86: COM1, driven through port I/O.

#include <ours/compiler.hpp>
#include <ours/types.hpp>

#include "arch.hpp"

namespace ours::fake_kernel {
namespace {
    // COM1.  The kernel runs in its own image and cannot reach physload's
    // console, so it drives the UART itself.  No initialization: the boot
    // shim has already configured the port, and re-initializing it here
    // would hide a chain that got this far with a broken console.
    //
    // This is the same port ours::phys::Handoff::uart names -- kept as its
    // own constant rather than read from the handoff because the first
    // write happens before the handoff pointer is even checked for
    // null, the same way a real kernel's own bootstrap console has to
    // assume something before it has verified anything.  handoff->uart is
    // checked by handoff_names_console() instead, as a report on whether
    // physload's copy agrees with this one.
    constexpr u16 COM_PORT = 0x3F8;
    constexpr u16 COM_LINE_STATUS = COM_PORT + 5;
    constexpr u8 TRANSMITTER_EMPTY = 1U << 5;

    FORCE_INLINE
    auto outb(u16 const port, u8 const value) -> void {
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    FORCE_INLINE
    auto inb(u16 const port) -> u8 {
        u8 value;
        __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }
} // namespace

auto write_char(char const character) -> void {
    while ((inb(COM_LINE_STATUS) & TRANSMITTER_EMPTY) == 0) {
    }
    outb(COM_PORT, static_cast<u8>(character));
}

[[noreturn]]
auto halt() -> void {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

auto handoff_names_console(phys::UartHandoff const &uart) -> bool {
    return uart.kind == phys::UartKind::X86Pio16550 && uart.base == COM_PORT;
}
} // namespace ours::fake_kernel
