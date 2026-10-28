// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// Entry stub for the fake kernel on riscv.  As on x86, a hand-written
// minimum: physboot calls the kernel with one argument, the handoff pointer
// in a0, and this only moves onto the image's own stack and passes it on.

.section .text.boot, "ax", @progbits
.balign 4

// [[noreturn]] auto _start(ours::phys::Handoff *handoff) -> void
.globl _start
.type _start, @function
_start:
    // Mask interrupts.  This image installs no trap vector, so one taken here
    // would go wherever stvec was last pointed.
    csrci   sstatus, 0x2                // SSTATUS.SIE

    // physboot hands over on its own stack, mapped in the address space it
    // built.  Switch to this image's stack before touching anything.  lla is
    // pc-relative, so it yields the address the image runs at, wherever
    // physboot relocated it.
    lla     sp, FAKE_KERNEL_STACK_END

    // The root of the call stack: no frame above this one.
    mv      s0, zero

    // a0 already holds the handoff; the ABI needs no shuffling.
    call    fake_kernel_main

    // fake_kernel_main is noreturn.  Should it ever return, stop here rather
    // than falling into whatever follows in .text.
0:
    wfi
    j       0b
.size _start, . - _start

.section .bss.boot, "aw", @nobits
.balign 16
FAKE_KERNEL_STACK:
    .skip 4096 * 4
.globl FAKE_KERNEL_STACK_END
FAKE_KERNEL_STACK_END:

// //kernel/kernel.ld spells its ENTRY() as MAIN_ENTRY; every kernel image
// has to publish that alias.
.globl MAIN_ENTRY
.set MAIN_ENTRY, _start

.section .note.GNU-stack, "", @progbits
