/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The fake kernel's body: check the handoff, say so on COM1, halt.
/// See README.md for why this target exists.

#include <ours/compiler.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/types.hpp>

namespace {
// COM1.  The kernel runs in its own image and cannot reach physload's
// console, so it drives the UART itself.  No initialization: the boot
// shim has already configured the port, and re-initializing it here
// would hide a chain that got this far with a broken console.
//
// This is the same port ours::phys::Handoff::uart names -- kept as its
// own constant rather than read from the handoff because the first
// write below happens before the handoff pointer is even checked for
// null, the same way a real kernel's own bootstrap console has to
// assume something before it has verified anything.  handoff->uart is
// checked further down instead, as a report on whether physload's copy
// agrees with this one.
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

auto write_char(char const character) -> void {
    while ((inb(COM_LINE_STATUS) & TRANSMITTER_EMPTY) == 0) {
    }
    outb(COM_PORT, static_cast<u8>(character));
}

auto write(char const *text) -> void {
    for (; *text != '\0'; ++text) {
        if (*text == '\n') {
            write_char('\r');
        }
        write_char(*text);
    }
}

auto write_hex(u64 value) -> void {
    char buffer[16];
    for (auto index = usize{0}; index < sizeof(buffer); ++index) {
        auto const nibble = static_cast<u8>(value & 0xF);
        buffer[sizeof(buffer) - 1 - index] =
            static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
        value >>= 4;
    }
    write("0x");
    for (auto const character: buffer) {
        write_char(character);
    }
}

auto write_line(char const *label, u64 const value) -> void {
    write(label);
    write_hex(value);
    write("\n");
}

[[noreturn]]
auto halt() -> void {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
} // namespace

/// Entered from entry-x86.S with the pointer physboot passed to the kernel.
extern "C" [[noreturn]]
auto fake_kernel_main(ours::phys::Handoff const *handoff) -> void {
    write("\n[fake-kernel] entered\n");

    if (handoff == nullptr) {
        write("[fake-kernel] FAIL: null handoff\n");
        halt();
    }

    write_line("[fake-kernel] handoff at   ", reinterpret_cast<u64>(handoff));
    write_line("[fake-kernel] magic        ", handoff->magic);

    if (handoff->magic != ours::phys::Handoff::MAGIC) {
        write("[fake-kernel] FAIL: bad handoff magic\n");
        halt();
    }

    // The things physload/physboot are responsible for filling in.
    // Reporting them before verify() means a failure names which one is
    // wrong instead of just saying the handoff is bad.
    write_line("[fake-kernel] acpi rsdp    ",
               handoff->acpi_rsdp.is_some() ? handoff->acpi_rsdp.value() : 0);
    write_line("[fake-kernel] kernel at    ", handoff->mem.kernel_load_addr);
    write_line("[fake-kernel] memory ranges", handoff->mem.ranges.size());
    write_line("[fake-kernel] uart kind    ", static_cast<u64>(handoff->uart.kind));
    write_line("[fake-kernel] uart base    ", handoff->uart.base);

    if (handoff->mem.ranges.empty()) {
        write("[fake-kernel] FAIL: memory map is empty\n");
        halt();
    }
    if (!handoff->boot_package.valid()) {
        write("[fake-kernel] FAIL: boot package ranges inconsistent\n");
        halt();
    }
    // Proves the handoff plumbing actually carried this, not just that this
    // file's own hardcoded COM_PORT happens to work regardless.
    if (handoff->uart.kind != ours::phys::UartKind::X86Pio16550 || handoff->uart.base != COM_PORT) {
        write("[fake-kernel] FAIL: handoff uart does not match the port in use\n");
        halt();
    }

    // The line the boot test greps for.  Getting here means physload parsed
    // the OBI, brought up boot memory, loaded and bound physboot, and
    // physboot loaded, relocated and entered a kernel out of the package.
    write("[fake-kernel] handoff ok\n");
    halt();
}
