/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_HANDLE_HPP
#define OURS_HANDLE_HPP 1

// The handle-table calls and their one limit, one domain in one file: a
// program that closes and duplicates handles includes this and nothing else
// of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// The most live handles one process may have created, wherever they are now:
/// in its own table, in another process's, or queued in a message on the
/// way. Creating one past it answers NoResource; closing one gives the room
/// back.
///
/// Counted against the creator, not the holder, so that sending handles
/// away does not make room for more: a process that makes channels and
/// writes their ends into a channel nobody reads would otherwise fill the
/// system-wide arena from a table that always looks empty. A quarter of
/// today's arena, so that no one process can exhaust it alone; it grows when
/// the arena does.
inline constexpr auto HANDLE_MAX_PER_PROCESS = u32{1024};

// The handle calls, as a program calls them: generated from
// //system/public/ours/syscalls/handle.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/handle.inc>
} // namespace ours

#endif // OURS_HANDLE_HPP
