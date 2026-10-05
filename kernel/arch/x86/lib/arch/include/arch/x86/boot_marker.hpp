/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_BOOT_MARKER_HPP
#define ARCH_X86_BOOT_MARKER_HPP 1

#include <arch/x86/io.hpp>

namespace arch {
/// Writes |c| to COM1's data port: no lock, and no wait for the transmitter,
/// so that it works at any point of the boot, whatever state the logger is in.
inline auto boot_marker(char const c) -> void {
    outb(0x3F8, static_cast<u8>(c));
}
} // namespace arch

#endif // ARCH_X86_BOOT_MARKER_HPP
