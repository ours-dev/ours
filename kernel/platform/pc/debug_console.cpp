/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/debug_console.hpp>

#include <arch/spin_hint.hpp>
#include <arch/x86/io.hpp>

namespace ours::platform {
namespace {
    constexpr u16 COM1_PORT = 0x3F8;

    auto receive_ready() -> bool {
        return (arch::inb(COM1_PORT + 5) & 0x01) != 0;
    }

    auto receive_byte() -> char {
        return static_cast<char>(arch::inb(COM1_PORT));
    }

    /// The line status register's "transmitter holding register empty" bit:
    /// the UART has room for the next byte.
    auto transmit_ready() -> bool {
        return (arch::inb(COM1_PORT + 5) & 0x20) != 0;
    }

    auto normalize(char const byte) -> char {
        if (byte == '\r') {
            return '\n';
        }
        return byte;
    }
} // namespace

auto debug_console_read(char *const buffer, usize const capacity, usize *const actual) -> Status {
    if (capacity == 0) {
        *actual = 0;
        return Status::Ok;
    }

    usize count = 0;
    while (count == 0) {
        if (receive_ready()) {
            buffer[count++] = normalize(receive_byte());
            break;
        }
        arch::spin_hint();
    }

    while (count < capacity && receive_ready()) {
        buffer[count++] = normalize(receive_byte());
    }
    *actual = count;
    return Status::Ok;
}

/// COM1, one byte at a time, each written once the UART has taken the one
/// before it.
auto debug_console_write(char const *const s, usize const n) -> void {
    for (usize i = 0; i < n; ++i) {
        while (!transmit_ready()) {
        }
        arch::outb(COM1_PORT, static_cast<u8>(s[i]));
    }
}
} // namespace ours::platform