/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The fake kernel's body: check the handoff, say so on the console, halt.
/// The console and the halt are the architecture's (arch.hpp).
/// See README.md for why this target exists.

#include <ours/phys/handoff.hpp>
#include <ours/types.hpp>

#include "arch.hpp"

namespace {
using ours::fake_kernel::halt;
using ours::fake_kernel::write_char;

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
} // namespace

/// Entered from entry_<cpu>.S with the pointer physboot passed to the kernel.
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
    write_line("[fake-kernel] device tree  ",
               handoff->device_tree.is_some() ? handoff->device_tree.value() : 0);
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
    // image's own idea of the console happens to work regardless.
    if (!ours::fake_kernel::handoff_names_console(handoff->uart)) {
        write("[fake-kernel] FAIL: handoff uart does not match the console in use\n");
        halt();
    }

    // The line the boot test greps for.  Getting here means physload parsed
    // the OBI, brought up boot memory, loaded and bound physboot, and
    // physboot loaded, relocated and entered a kernel out of the package.
    write("[fake-kernel] handoff ok\n");
    halt();
}
