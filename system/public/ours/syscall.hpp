/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_HPP
#define OURS_SYSCALL_HPP 1

// The system-call ABI: the contract between a user program and the kernel's
// entry stub.  It lives in system/public because both sides have to agree on
// it -- the kernel builds its dispatch table from these numbers, and a user
// program puts one of them in the syscall-number register.
//
// Written so an assembler can read it.  kernel/arch/x86/kernel/syscall.S needs
// OURS_SYSCALL_COUNT for its range check, and the assembler will not survive a
// C++ declaration, so the count is a macro and the C++ spelling below is a
// mirror rather than the definition.

/// How many system-call numbers exist.  Numbers are dense from zero, so this
/// is also one past the largest valid number.
///
/// The dispatch table is deliberately one entry LONGER than this: index
/// OURS_SYSCALL_COUNT is a sentinel slot holding the out-of-range answer, and
/// the entry stub clamps a user-supplied number onto it rather than branching
/// around the call.  See SYSCALL_TABLE in //kernel/lib/syscall/table.cpp.
#define OURS_SYSCALL_COUNT 256

#ifdef __cplusplus

#include <ours/types.hpp>

namespace ours {
/// The C++ spelling of OURS_SYSCALL_COUNT above.
inline constexpr auto SYSCALL_COUNT = usize{OURS_SYSCALL_COUNT};
} // namespace ours

#endif // __cplusplus

#endif // OURS_SYSCALL_HPP
