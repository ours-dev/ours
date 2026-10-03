/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef RUNTIME_DEBUG_HPP
#define RUNTIME_DEBUG_HPP 1

#include <initializer_list>
#include <string_view>

#include <ours/status.hpp>

/// A program's way to say what it is doing while the system is brought up:
/// ours_debug_write, made fit to call. In the runtime because every program
/// links it and every program prints; they came from //system/lib/ou, removed
/// once the user decided ours_* is the API a program calls.
namespace runtime {
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
/// truncating. Splitting the text to fit, at MAX_DEBUG_WRITE
/// (<ours/syscall.hpp>), is the kind of thing every caller would otherwise
/// write for itself.
auto debug_write(std::string_view text) -> ours::Status;

/// Writes |pieces| one after another, then a newline, as one line.
///
/// One line, one system call, whenever it fits in MAX_DEBUG_WRITE: the kernel
/// writes each call's bytes out under its logger's lock, so a line that goes
/// out in one call cannot be split by another program printing at the same
/// moment. It used to go out as the text and then the newline, and a caller
/// building a line from parts made a call per part; with two processes on
/// two CPUs the console read `shell: close(bootstrap) = init: shell says
/// "pongOk"`. A line longer than the limit still goes out, in several calls,
/// and can then interleave.
auto debug_line(std::initializer_list<std::string_view> pieces) -> ours::Status;

/// debug_line() of a single piece.
auto debug_line(std::string_view text) -> ours::Status;

/// |value| in decimal, written into |buffer| and returned as the piece that
/// holds it, for a debug_line: `runtime::debug_line({"made ", decimal(n, text)})`.
///
/// Ten characters is every u32. The caller owns the buffer, so the view stays
/// valid as long as the line being built does, with nothing allocated. Here
/// because there is nothing else to format a number with: the C library a
/// program links is mem* and strlen (//system/lib/libc), with no printf.
auto decimal(u32 value, char (&buffer)[10]) -> std::string_view;

/// |value| in hexadecimal with a leading "0x" and no leading zeros, written
/// into |buffer| as decimal() does: `runtime::debug_line({"at ", hex(address, text)})`.
/// Eighteen characters is every u64 with its prefix.
auto hex(u64 value, char (&buffer)[18]) -> std::string_view;
} // namespace runtime

#endif // #ifndef RUNTIME_DEBUG_HPP
