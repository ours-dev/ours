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
// u32 x86_user_copy(void *dst, void const *src, usize len, VirtAddr *resume)
//
//   %rdi  dst     -- rep movs writes through it and advances it
//   %rsi  src     -- rep movs reads through it and advances it
//   %rdx  len     -- split into quadwords (%rcx for rep movsq) and a byte tail
//   %rcx  resume  -- the running thread's data-fault resume slot; moved to
//                    %r10 on entry, because rep movs needs %rcx
//
//   returns 0 when every byte was copied, 1 when a page fault on the user
//   side could not be resolved. The C++ caller turns 1 into InvArgs; the
//   assembly does not spell a Status.
//
// Fault recovery (design 4.6). Before it touches user memory the routine
// stores the address of its fault exit in *resume, and it clears the slot
// before it returns, on either path. If the copy faults on a user address and
// the page-fault handler cannot resolve it, the handler finds the slot set
// and the faulting instruction inside [x86_user_copy_begin,
// x86_user_copy_end), and instead of panicking it rewrites the frame's
// instruction pointer to the slot's address and returns. The copy then
// resumes at its own fault exit, closes the SMAP window, clears the slot and
// returns 1. The address check is what keeps anything else from being sent
// there: on a CPU without SMAP, an interrupt taken in the middle of the copy
// whose handler touched a user address would otherwise find the slot set too.
//
// Two invariants make that safe, and both are local to this file:
//
//   - Between setting the slot and clearing it, nothing calls and nothing
//     touches the stack. The fault exit is reached with the stack exactly as
//     it was at the faulting instruction, which is exactly as it was on entry,
//     so its `ret` returns to the caller.
//   - The fault exit uses only %r10 (the slot) and %rax (the result). %r10
//     survives the fault because the exception entry saves every general
//     register into the frame and restores it on the way out.
//
// The direction flag is clear on entry, as the SysV ABI promises every
// function; SYSCALL clears it (IA32_FMASK) and so does every interrupt stub.
//
// Zircon counterpart: kernel/arch/x86/user-copy/user-copy.S, assembled once
// per variant with -DSMAP; Linux's is arch/x86/lib/copy_user_64.S.

#include <ours/arch/asm.hpp>

.macro user_copy name, smap
.function \name, global, nosection=nosection
    movq %rcx, %r10
    leaq .Lfault_\name(%rip), %rax
    movq %rax, (%r10)
.if \smap
    stac
.endif
    movq %rdx, %rcx
    shrq $3, %rcx
    rep movsq
    movl %edx, %ecx
    andl $7, %ecx
    rep movsb
    xorl %eax, %eax
.Ldone_\name:
.if \smap
    clac
.endif
    movq $0, (%r10)
    ret

.Lfault_\name:
    movl $1, %eax
    jmp .Ldone_\name
.end_function
.endm

// Both variants in one section, one after the other, so that the range they
// occupy together -- the only instructions a resumable user fault may come
// from -- is one contiguous interval with a label at each end.
.section .text.x86_user_copy, "ax", @progbits

.label x86_user_copy_begin, global
user_copy x86_user_copy, 0
user_copy x86_user_copy_smap, 1
.label x86_user_copy_end, global

.section .note.GNU-stack, "", @progbits
