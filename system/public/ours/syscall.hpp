/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_HPP
#define OURS_SYSCALL_HPP 1

// The constants that cross domains: the sentinel a deadline argument takes,
// and the name length every call that takes a name cuts to. Each is named
// once so that both sides compile against the same value.
//
// A domain's own option words and limits live with that domain's calls, in
// its own header (<ours/port.hpp>, <ours/vma.hpp>, <ours/channel.hpp>, ...),
// one file with the generated declarations a program calls -- not gathered
// here. What is nowhere in system/public is the call numbers. A program names
// a call by its vDSO symbol (<ours/vdso.hpp>), so the numbers are a private
// agreement between the vDSO and the kernel's dispatch table (design D8).
// Both are generated from //system/public/ours/syscalls/*.gni, into a header
// only those two can see (//system/public:syscalls.private). Publishing the
// numbers would only have invited the one thing the vDSO exists to prevent.

#include <ours/types.hpp>

#include <limits>

namespace ours {
/// A deadline that never arrives: wait until the condition holds, however
/// long that takes.
///
/// Its own constant rather than the kernel's INFINITE_TIME, which is a chrono
/// TimePoint the kernel's timers are written against. This one is the plain
/// number a program passes in a register.
inline constexpr auto INSTANT_INFINITE = std::numeric_limits<Instant>::max();

/// The size of an object's name, its terminator included: a process, thread,
/// VMO or resource keeps at most the first 31 bytes of the name it is given,
/// and process_create and thread_create cut a longer one rather than refuse
/// it.
///
/// Not NAME_MAX, which <limits.h> defines as a macro (the longest file name,
/// 255 in musl), and which would replace this name in any program that
/// includes both.
inline constexpr auto MAX_NAME_LEN = usize{32};

} // namespace ours

#endif // OURS_SYSCALL_HPP
