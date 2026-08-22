// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Entry stub for the fake kernel.  Deliberately a hand-written minimum
// rather than a copy of //kernel/arch/x86/kernel/start.S: that file installs
// a GDT, publishes the page-table root and jumps into the kernel proper,
// none of which exists here.  What it does share is the calling convention
// physboot uses -- one argument, the handoff pointer, in %rdi.

.section .text.boot, "ax", @progbits
.align 16

// [[noreturn]] auto _start(ours::phys::Handoff *handoff) -> void
.globl _start
.type _start, @function
_start:
    cli

    // physboot hands over on its own stack, mapped in the address space it
    // built.  Switch to this image's stack before touching anything.
    lea g_fake_kernel_stack_end(%rip), %rsp
    xor %ebp, %ebp

    // %rdi already holds the handoff; the ABI needs no shuffling.
    call fake_kernel_main

    // fake_kernel_main is noreturn.  Should it ever return, stop here rather
    // than falling into whatever follows in .text.
0:
    hlt
    jmp 0b
.size _start, . - _start

.section .bss.boot, "aw", @nobits
.align 16
g_fake_kernel_stack:
    .skip 4096 * 4
.globl g_fake_kernel_stack_end
g_fake_kernel_stack_end:

// //kernel/kernel.ld spells its ENTRY() as MAIN_ENTRY; every kernel image
// has to publish that alias.
.globl MAIN_ENTRY
.set MAIN_ENTRY, _start

.section .note.GNU-stack, "", @progbits
