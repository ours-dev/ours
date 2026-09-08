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

/// Write a string to the kernel's debug console.
///
/// Arguments follow the ordinary x86-64 SysV argument registers, which is
/// what makes an inline `syscall` in a user program readable: %rdi is the
/// buffer, %rsi its length in bytes.  %rax carries the number in and the
/// ours::Status out.  Note %rcx and %r11 are NOT arguments and never can be:
/// the SYSCALL instruction overwrites both before the kernel sees them, which
/// is why the fourth SysV argument register is %r10 here and not %rcx.
///
/// This is a debug facility, not a console: it exists so that the first user
/// program can prove it reached user mode and got back, and it has no
/// counterpart in a finished system.  Zircon's zx_debug_write is the same
/// thing for the same reason.
#define OURS_SYS_DEBUG_WRITE 0

#ifdef __cplusplus

#include <ours/types.hpp>

namespace ours {
/// The C++ spelling of OURS_SYSCALL_COUNT above.
inline constexpr auto SYSCALL_COUNT = usize{OURS_SYSCALL_COUNT};

/// The C++ spelling of the numbers above.
enum class SyscallNumber : usize {
    DebugWrite = OURS_SYS_DEBUG_WRITE,
};
} // namespace ours

#endif // __cplusplus

#endif // OURS_SYSCALL_HPP
