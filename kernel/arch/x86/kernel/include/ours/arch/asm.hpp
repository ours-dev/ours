/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ASM_HPP
#define OURS_ARCH_ASM_HPP 1

#include <arch/asm.hpp>

#if defined(__ASSEMBLER__) || defined(__ASSEMBLY__)
.macro push_reg register
    pushq \register
.endm

.macro pop_reg register
    popq \register
.endm

.macro push_val value
    pushq \value
.endm

.macro all_cfi_same_value
    .cfi_same_value %rax
    .cfi_same_value %rbx
    .cfi_same_value %rcx
    .cfi_same_value %rdx
    .cfi_same_value %rsi
    .cfi_same_value %rdi
    .cfi_same_value %rbp
    .cfi_same_value %r8
    .cfi_same_value %r9
    .cfi_same_value %r10
    .cfi_same_value %r11
    .cfi_same_value %r12
    .cfi_same_value %r13
    .cfi_same_value %r14
    .cfi_same_value %r15
.endm

.macro save_general_registers
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
.endm

.macro zero_general_registers
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
.endm

.macro restore_general_registers
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
.endm

.macro add_to_sp bytes
    addq $\bytes, %rsp
.endm
#endif

#define KERNEL_LOAD_ADDR    0x0000000000100000
#define KERNEL_BASE         0xffffff8000100000

#define EALY_IDT_ENTRIES    32
#define PHYS(x) ((x) - (KERNEL_BASE - KERNEL_LOAD_ADDR))

#endif // #ifndef OURS_ARCH_ASM_HPP
