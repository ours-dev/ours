/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <runtime/debug.hpp>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace runtime {
using ours::Status;

auto debug_write(std::string_view const text) -> Status {
    auto const *at = text.data();
    auto left = text.size();

    // The kernel refuses a call longer than MAX_DEBUG_WRITE rather than
    // truncating it, so the split happens here. The constant comes from
    // <ours/syscall.hpp>, which both sides of the boundary include: a limit
    // the caller had to guess would be a limit it discovered as OutOfRange.
    while (left != 0) {
        auto const chunk = left < ours::MAX_DEBUG_WRITE ? left : ours::MAX_DEBUG_WRITE;
        auto const status = ours_debug_write(at, chunk);
        if (Status::Ok != status) {
            // Reported rather than retried. Whatever stopped the kernel from
            // taking this chunk will stop it taking the next one, and a caller
            // that cannot print is usually a caller that has nothing left to
            // say anyway.
            return status;
        }
        at += chunk;
        left -= chunk;
    }
    return Status::Ok;
}

auto debug_line(std::initializer_list<std::string_view> const pieces) -> Status {
    usize total = 1; // the newline
    for (auto const &piece: pieces) {
        total += piece.size();
    }

    if (total <= ours::MAX_DEBUG_WRITE) {
        // Assembled here and handed over in one call, which is what keeps
        // the line whole (see the declaration).
        char line[ours::MAX_DEBUG_WRITE];
        usize at = 0;
        for (auto const &piece: pieces) {
            for (usize i = 0; i < piece.size(); ++i) {
                line[at] = piece[i];
                at += 1;
            }
        }
        line[at] = '\n';
        at += 1;
        return ours_debug_write(line, at);
    }

    for (auto const &piece: pieces) {
        if (auto const status = debug_write(piece); Status::Ok != status) {
            return status;
        }
    }
    return debug_write("\n");
}

auto debug_line(std::string_view const text) -> Status {
    return debug_line({text});
}

auto decimal(u32 value, char (&buffer)[10]) -> std::string_view {
    auto at = sizeof(buffer);
    do {
        buffer[--at] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return {buffer + at, sizeof(buffer) - at};
}

auto hex(u64 value, char (&buffer)[18]) -> std::string_view {
    constexpr char DIGITS[] = "0123456789abcdef";
    auto at = sizeof(buffer);
    do {
        buffer[--at] = DIGITS[value % 16];
        value /= 16;
    } while (value != 0);
    buffer[--at] = 'x';
    buffer[--at] = '0';
    return {buffer + at, sizeof(buffer) - at};
}
} // namespace runtime
