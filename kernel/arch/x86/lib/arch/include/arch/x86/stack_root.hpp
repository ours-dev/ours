/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_STACK_ROOT_HPP
#define ARCH_X86_STACK_ROOT_HPP 1

#include <arch/x86/interrupt.hpp>
#include <arch/x86/syscall.hpp>

#include <array>

// The return addresses of the calls the entry stubs make into C++, labelled
// where the calls are (syscall.S, exception.S).
extern "C" char const x86_syscall_dispatch_return[];
extern "C" char const x86_syscall_signals_return[];
extern "C" char const x86_exception_return[];

namespace arch {
/// A place a kernel stack begins: a return address, and how far below the
/// top of the thread's kernel stack the call that pushed it was made.
struct StackRoot final {
    usize return_address;
    usize depth;
};

/// The places a kernel stack begins when the thread came in from user mode.
/// An entry from user mode lands on the top of the thread's kernel stack --
/// TSS.sp0 for an interrupt or exception, the per-CPU kernel stack pointer
/// for SYSCALL, both set to it at every switch -- and the stub builds its
/// frame there and calls into C++, so the return address sits exactly the
/// frame's size below the top. The exception stub is also entered from
/// kernel mode, nested deeper; its frame is then not at the top, the depth
/// does not match, and the stack goes on below it. A kernel thread's stack
/// begins at Thread::trampoline instead, whose null return address ends a
/// walk without a root.
inline auto stack_roots() -> std::array<StackRoot, 3> {
    return {{
        {reinterpret_cast<usize>(x86_syscall_dispatch_return), sizeof(SyscallFrame)},
        {reinterpret_cast<usize>(x86_syscall_signals_return), sizeof(SyscallFrame)},
        {reinterpret_cast<usize>(x86_exception_return), sizeof(IrqFrame)},
    }};
}
} // namespace arch

#endif // ARCH_X86_STACK_ROOT_HPP
