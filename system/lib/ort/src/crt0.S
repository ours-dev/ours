// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The entry point of every user program that links the runtime, x86-64.
//
// Assembly for two reasons. The kernel enters with %rsp at 8 below a 16-byte
// boundary (design 3.4: as if a call had just pushed a return address), and
// only assembly can make that 16 again before the first call without relying
// on what a compiler assumes about its own entry. And main() is called from
// here and nowhere else: C++ forbids a program to call main
// ([basic.start.main]), so the one call to it is written below the language.
//
// [[noreturn]] void _start(ours::Uandle bootstrap, VirtAddr vdso_base)
//   %rdi  the bootstrap channel, Uandle::Invalid when the process has none
//   %rsi  where the vDSO is mapped
//
// Both go on untouched to ort_start() (start.cpp), which reads the
// processargs message and calls back into ort_call_main() below.

.section .text._start, "ax", @progbits
.balign 16
.globl _start
.type _start, @function
_start:
    // The outermost frame: a frame-pointer walk stops at a zero %rbp.
    xorl %ebp, %ebp
    andq $-16, %rsp
    call ort_start
    ud2
.size _start, . - _start

// int ort_call_main(int argc, char **argv)
//
// A tail jump, so main() returns straight to ort_start() with its code in
// %eax. The arguments are already where main() wants them. Hidden, like
// everything else in a program: it is the runtime's, not an export.
.section .text.ort_call_main, "ax", @progbits
.balign 16
.globl ort_call_main
.hidden ort_call_main
.type ort_call_main, @function
ort_call_main:
    jmp main
.size ort_call_main, . - ort_call_main

.section .note.GNU-stack, "", @progbits
