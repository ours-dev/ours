/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_SYSCALL_HPP
#define ARCH_SYSCALL_HPP 1

// Same shape as arch/interrupt.hpp: there is nothing generic to declare up
// front, because the frame a system-call entry stub builds is defined by the
// instruction that gets the kernel there (x86's SYSCALL leaves the return
// address in a register and does not touch the stack; other architectures
// trap differently) and by that architecture's own stub.  Each
// arch/<arch>/syscall.hpp therefore defines SyscallFrame outright.

#if defined(ARCH_X86)
#include <arch/x86/syscall.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/syscall.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/syscall.hpp>
#else

namespace arch {
struct SyscallFrame;
} // namespace arch

#endif

#endif // ARCH_SYSCALL_HPP
