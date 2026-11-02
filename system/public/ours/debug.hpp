/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_DEBUG_HPP
#define OURS_DEBUG_HPP 1

// The debug-console calls, their limits and where they live, one domain in
// one file: a program that prints while the system is brought up includes
// this and nothing else of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
/// The most bytes one debug_write call will copy.
///
/// A ceiling on one call, so that a program cannot hold a CPU inside the
/// kernel for as long as it likes by asking for a gigabyte of output. The
/// number is arbitrary; what is not arbitrary is that there is one, and that
/// both sides know the same one. It lives here rather than in the handler
/// because the caller has to split its text to fit -- a limit only the kernel
/// knows is a limit the caller discovers as Status::OutOfRange.
inline constexpr auto MAX_DEBUG_WRITE = usize{256};

/// The most bytes one debug_read call may return.
inline constexpr auto MAX_DEBUG_READ = usize{256};

// The debug calls, as a program calls them: generated from
// //system/public/ours/syscalls/debug.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/debug.inc>
} // namespace ours

#endif // OURS_DEBUG_HPP
