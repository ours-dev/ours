/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SYSCALL_HANDLERS_HPP
#define SYSCALL_HANDLERS_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// The typed system-call entry points.
///
/// These take arguments and return a Status like ordinary functions. Nothing
/// about a register frame reaches them: table.cpp holds one small shim per
/// call that pulls the arguments out of arch::SyscallFrame and puts the Status
/// back into it. That shim is the dispatch layer, and keeping it separate is
/// what lets these be read, reviewed and eventually tested as plain functions
/// -- the same split Zircon draws between its generated syscall wrappers and
/// the sys_* functions they call.
///
/// A pointer parameter here is a *user* pointer. It has been range-checked
/// against the user half of the address space by the time the callee runs, but
/// it is not otherwise trusted, and it must not be dereferenced by anything
/// that cannot survive the page under it going away.

/// Write |length| bytes of |buffer| to the kernel's debug console.
auto sys_debug_write(char const *buffer, usize length) -> Status;

/// Writes the kshell command registry into |buffer| as text, one command per
/// line ("name  help"), always NUL terminated.
///
/// Status::NoSpace when the registry did not fit; what did fit is still
/// terminated, so a caller can show it and ask for more room.
auto sys_command_list(char *buffer, usize length) -> Status;

} // namespace ours

#endif // SYSCALL_HANDLERS_HPP
