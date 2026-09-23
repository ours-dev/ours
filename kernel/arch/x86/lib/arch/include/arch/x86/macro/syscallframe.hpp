/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_MACRO_SYSCALLFRAME_HPP
#define ARCH_X86_MACRO_SYSCALLFRAME_HPP 1

// The register frame kernel/arch/x86/kernel/syscall.S builds on the kernel
// stack, as byte offsets from the frame pointer it hands the dispatch table.
//
// Same arrangement as arch/x86/macro/irqframe.hpp and for the same reason:
// the stub and the C++ struct have to agree on a layout that neither of them
// can derive from the other, so both are written against these macros and
// arch::SyscallFrame static_asserts itself against them.  Reordering a push
// in syscall.S without touching this file is then a build failure rather
// than a wrong register read at runtime.
//
// The order below is the *reverse* of the push order: the stack grows down,
// so the last value pushed (%rax) is the one at offset 0.

#define X86_SYSCALLFRAME_OFFSET_SCALE 8
#define X86_SYSCALLFRAME_OFFSET_AX (0 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_BX (1 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_CX (2 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_DX (3 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_SI (4 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_DI (5 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_BP (6 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R8 (7 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R9 (8 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R10 (9 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R11 (10 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R12 (11 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R13 (12 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R14 (13 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_R15 (14 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_IP (15 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_FLAGS (16 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_OFFSET_SP (17 * X86_SYSCALLFRAME_OFFSET_SCALE)
#define X86_SYSCALLFRAME_SIZE (18 * X86_SYSCALLFRAME_OFFSET_SCALE)

#endif // ARCH_X86_MACRO_SYSCALLFRAME_HPP
