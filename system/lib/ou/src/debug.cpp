/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/debug.hpp>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace ou {
using ours::Status;

auto debug_write(Str const text) -> Status {
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

auto debug_line(Str const text) -> Status {
    auto const status = debug_write(text);
    if (Status::Ok != status) {
        return status;
    }
    return debug_write("\n");
}
} // namespace ou
