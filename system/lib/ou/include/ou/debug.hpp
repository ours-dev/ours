/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_DEBUG_HPP
#define OU_DEBUG_HPP 1

#include <ou/str.hpp>

#include <ours/status.hpp>

namespace ou {
/// Writes |text| to the kernel's debug console.
///
/// A debug facility, not a console: it exists so a program can say what it is
/// doing while the system is being brought up, and it has no counterpart in a
/// finished system. Zircon's zx_debug_write is the same thing for the same
/// reason.
///
/// One call to this may become several traps. The kernel caps how much one
/// system call will copy -- it will not spend unbounded time in the kernel on
/// a program's say-so -- and answers OutOfRange past that instead of
/// truncating. Splitting the text to fit is the kind of thing every caller
/// would otherwise write for itself, out of a constant it would have to keep
/// in step with the kernel by hand.
auto debug_write(Str text) -> ours::Status;

/// debug_write() with a newline after it.
auto debug_line(Str text) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_DEBUG_HPP
