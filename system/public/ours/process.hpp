/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PROCESS_HPP
#define OURS_PROCESS_HPP 1

// The process calls, their options and their one limit, one domain in one
// file: a program that makes, starts and reads processes includes this and
// nothing else of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// process_create's options. None is the only value it accepts today; anything
/// else is InvArgs. There is no option for a process whose address space
/// another process can share: this ABI has no shared address spaces.
enum class ProcessCreateOptions : u32 {
    None = 0,
};

/// The most bytes one process_read_memory call may ask for: 64 MiB.
inline constexpr auto MAX_PROCESS_READ_MEMORY = usize{64} << 20;

// The process calls, as a program calls them: generated from
// //system/public/ours/syscalls/process.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/process.inc>
} // namespace ours

#endif // OURS_PROCESS_HPP
