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
    constexpr u16 kComPort = 0x3F8;
    constexpr u16 kComLineStatus = kComPort + 5;
    constexpr u8 kTransmitterEmpty = 1U << 5;

    FORCE_INLINE auto outb(u16 const port, u8 const value) -> void {
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    FORCE_INLINE auto inb(u16 const port) -> u8 {
        u8 value;
        __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }

    auto write_char(char const character) -> void {
        while ((inb(kComLineStatus) & kTransmitterEmpty) == 0) {
        }
        outb(kComPort, static_cast<u8>(character));
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

    NO_RETURN auto halt() -> void {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
} // namespace

/// Entered from entry-x86.S with the pointer physboot passed to the kernel.
NO_MANGLE NO_RETURN auto fake_kernel_main(ours::phys::Handoff const *handoff) -> void {
    write("\n[fake-kernel] entered\n");

    if (handoff == nullptr) {
        write("[fake-kernel] FAIL: null handoff\n");
        halt();
    }

    write_line("[fake-kernel] handoff at   ", reinterpret_cast<u64>(handoff));
    write_line("[fake-kernel] magic        ", handoff->magic);

    if (handoff->magic != ours::phys::Handoff::kMagic) {
        write("[fake-kernel] FAIL: bad handoff magic\n");
        halt();
    }

    // The three things physload/physboot are responsible for filling in.
    // Reporting them before verify() means a failure names which one is
    // wrong instead of just saying the handoff is bad.
    write_line("[fake-kernel] cpus         ", handoff->nr_cpus);
    write_line("[fake-kernel] acpi rsdp    ", handoff->acpi_rsdp);
    write_line("[fake-kernel] kernel at    ", handoff->mem.kernel_load_addr);
    write_line("[fake-kernel] bootmem low  ", handoff->mem.bootmem.min_address);
    write_line("[fake-kernel] bootmem high ", handoff->mem.bootmem.max_address);

    if (!handoff->mem.bootmem.valid()) {
        write("[fake-kernel] FAIL: boot-memory service table incomplete\n");
        halt();
    }
    if (!handoff->boot_package.valid()) {
        write("[fake-kernel] FAIL: boot package ranges inconsistent\n");
        halt();
    }

    // The line the boot test greps for.  Getting here means physload parsed
    // the OBI, brought up boot memory, loaded and bound physboot, and
    // physboot loaded, relocated and entered a kernel out of the package.
    write("[fake-kernel] handoff ok\n");
    halt();
}
