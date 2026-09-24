// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The copy between kernel and user memory: the only code in the kernel that
// touches a user page.
//
// Two variants of one routine, expanded from one macro:
//
//   x86_user_copy       for a CPU without SMAP. Nothing to open: ring 0 may
//                       touch user pages at any time, and the stac/clac this
//                       variant lacks would be #UD on such a CPU (Nehalem,
//                       the default QEMU model here, is one).
//   x86_user_copy_smap  for a CPU with SMAP, which refuses a supervisor
//                       access to a user page unless RFLAGS.AC is set. The
//                       copy is bracketed by stac/clac, and those two
//                       instructions bracket nothing else in the kernel.
//
// The C++ side (user_copy.cpp) picks one by feature_has_smap. Zircon picks by
// patching the code at boot, which this kernel does not build with
// (ours_kernel_has_code_patches = false); a cached flag is the same choice
// made one call later.
//
// Why assembly rather than memcpy between an inline stac and clac, which is
// what this used to be. That was correct -- the "memory" clobbers kept the
// copy's loads and stores inside the window -- but the window then held a call
// to the kernel's general memcpy, the same function every other caller runs.
// The fault recovery (design 4.6) has to send a copy that faults to a known
// address with known registers, and to recognise a fault as the copy's by
// where it happened; a compiler-generated memcpy promises neither. Written
// out, the window holds exactly the copy, and the instructions a user fault
// may come from are the few in this file.
//
// An interrupt taken inside the window does not inherit it: every entry stub
// begins with clac (idt.S), and iretq puts AC back from the frame on the way
// out. A page fault the copy itself raises is handled the same way, with AC
// clear, and the copy resumes with it set.
//
// void x86_user_copy(void *dst, void const *src, usize len)
//
//   %rdi  dst   -- rep movs writes through it and advances it
//   %rsi  src   -- rep movs reads through it and advances it
//   %rdx  len   -- split into quadwords (%rcx for rep movsq) and a byte tail
//
// The direction flag is clear on entry, as the SysV ABI promises every
// function; SYSCALL clears it (IA32_FMASK) and so does every interrupt stub.
//
// Zircon counterpart: kernel/arch/x86/user-copy/user-copy.S, assembled once
// per variant with -DSMAP; Linux's is arch/x86/lib/copy_user_64.S.

#include <ours/arch/asm.hpp>

.macro user_copy name, smap
.function \name, global, nosection=nosection
    movq %rdx, %rcx
    shrq $3, %rcx
.if \smap
    stac
.endif
    rep movsq
    movl %edx, %ecx
    andl $7, %ecx
    rep movsb
.if \smap
    clac
.endif
    ret
.end_function
.endm

// Both variants in one section, one after the other, so that the range they
// occupy together is contiguous.
.section .text.x86_user_copy, "ax", @progbits

user_copy x86_user_copy, 0
user_copy x86_user_copy_smap, 1

.section .note.GNU-stack, "", @progbits
