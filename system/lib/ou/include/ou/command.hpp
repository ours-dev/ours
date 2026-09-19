/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_COMMAND_HPP
#define OU_COMMAND_HPP 1

#include <ou/str.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Fetches the kernel's command registry into |buffer| as text, one command
/// per line ("name  help"), always NUL terminated.
///
/// Status::NoSpace when the registry did not fit; what did fit is still
/// terminated, so a caller can show that much and ask for more room.
auto command_list(char *buffer, usize capacity) -> ours::Status;

/// The same, for a buffer whose size the compiler already knows.
///
/// Preferred at every call site that has an array: the capacity cannot be
/// stated wrongly, which is the one way to get this call wrong.
template <usize N>
auto command_list(char (&buffer)[N]) -> ours::Status {
    return command_list(buffer, N);
}

/// The text command_list() left in |buffer|, as far as its terminator.
template <usize N>
auto command_text(char const (&buffer)[N]) -> Str {
    return Str::from_c_str(buffer, N);
}
} // namespace ou

#endif // #ifndef OU_COMMAND_HPP
