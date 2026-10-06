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
// The C++ side (user_copy.cpp) picks one by FEATURE_HAS_SMAP. This kernel
// does not build with code patches (ours_kernel_has_code_patches = false), so
// the choice is a cached flag tested on each call rather than patched in at
// boot, and there is only the quadword copy: a `rep movsb` variant, which
// pays where the CPU makes it fast (ERMS, FSRM), would be one more choice to
// make the same way.
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
// X86UserCopyRet x86_user_copy(void *dst, void const *src, usize len,
//                               VirtAddr *resume, VirtAddr resume_mask)
//
//   %rdi  dst          -- rep movs writes through it and advances it
//   %rsi  src          -- rep movs reads through it and advances it
//   %rdx  len          -- split into quadwords (%rcx for rep movsq) and a
//                         byte tail
//   %rcx  resume       -- the running thread's data-fault resume slot; moved
//                         to %r10 on entry, because rep movs needs %rcx
//   %r8   resume_mask  -- ANDed into the fault exit's address before it is
//                         stored: all ones to have faults resolved in place,
//                         or with UserCopyState::RUN_FAULT_HANDLER (bit 63)
//                         clear to have them captured instead
//
//   returns struct X86UserCopyRet { u32 faulted; u32 cause; VirtAddr va; },
//   sixteen bytes of integers and so returned in %rax:%rdx under the SysV
//   ABI: %eax is 0 when every byte was copied and 1 when the copy took its
//   fault exit, the upper half of %rax the fault's VmfCause, %rdx the
//   faulting address. The last two are only meaningful for a capturing copy
//   that faulted -- only then did the page-fault handler put them in %rcx and
//   %rdx; otherwise they are whatever the copy left there. The C++ caller
//   turns a fault into InvArgs; the assembly does not spell a Status.
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
//   - The fault exit uses only %r10 (the slot), %rax (the result), and the
//     %rcx and %rdx the page-fault handler wrote into the frame. %r10 survives
//     the fault because the exception entry saves every general register into
//     the frame and restores it on the way out; %rcx and %rdx come back the
//     same way, rewritten.
//
// Fault capture. With RUN_FAULT_HANDLER clear in the stored exit address,
// the page-fault handler does not try to resolve a fault inside the copy at
// all. It writes the faulting address into the frame's %rdx and the fault's
// VmfCause into its %rcx, sets the instruction pointer to the exit -- with
// bit 63 put back, since clearing it made the address non-canonical -- and
// returns. It enables nothing and takes no lock on the way, so a capturing
// copy may run where resolving a fault could not: under a lock, with
// preemption or interrupts off. The exit then only has to pack %rcx into the
// upper half of %rax, and %rdx is already where the return struct wants it.
// Those two registers are chosen for exactly that.
//
// The direction flag is clear on entry, as the SysV ABI promises every
// function; SYSCALL clears it (IA32_FMASK) and so does every interrupt stub.

#include <ours/arch/asm.hpp>

.macro user_copy name, smap
.function \name, global, nosection=nosection
    movq %rcx, %r10
    leaq .Lfault_\name(%rip), %rax
    andq %r8, %rax
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
    shlq $32, %rcx
    movl $1, %eax
    orq %rcx, %rax
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
