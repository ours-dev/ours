/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/debug_console.hpp>

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
        __asm__ volatile("pause");
    }

    while (count < capacity && receive_ready()) {
        buffer[count++] = normalize(receive_byte());
    }
    *actual = count;
    return Status::Ok;
}
} // namespace ours::platform