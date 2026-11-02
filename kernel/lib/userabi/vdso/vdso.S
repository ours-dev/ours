// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The vDSO: the only image in a user address space whose `syscall` the
// kernel accepts.
//
// The shape of a stub is deliberate: put the number in %eax, trap, and label
// the return address, because that label is what an "approved call site"
// means.
//
// This file used to carry an export table at the ELF entry point as well, so
// that a program with no dynamic linker could find the stubs by offset. It is
// gone: the image is linked -shared now, so the stubs are in .dynsym, and
// whoever loads a program binds its imports to them (elfload::Binder, from the
// kernel's userabi::launch() or the user-space launcher). The ELF format's own
// answer, in place of a private one.
//
// Written in assembly rather than C++ because of the `syscall` instruction and
// because the return-address labels have to be exact: a stub that did anything
// besides move the number into %eax and trap would be a place for the ABI to
// drift.
//
// The stubs themselves are no longer written here. Each is one line of
// syscall_abi/vdso_stubs.inc, generated from //system/public/ours/syscalls.gni
// (design 4.7), invoking the one macro below. What is left by hand is the
// shape of a stub, written once, rather than eight copies of it that had to
// agree with each other and with the kernel.

// ---- The stub --------------------------------------------------------------
//
// ours_syscall name, sysret, number, nargs, noreturn, blocking
//
//   name      the exported entry point, ours_<call>
//   sysret    the label on the instruction after `syscall`
//   number    the call number, private to this image and the kernel (D8)
//   nargs     how many arguments the call takes
//   noreturn  1 when the call never comes back
//   blocking  1 when the call may wait, and is retried on InternalIrqRetry
//
// The entry point is `.globl` with default visibility, which is what puts it
// in .dynsym: this object is linked -shared, and .dynsym is how a program that
// merely *calls* ours_debug_write() gets bound to it at load time, with no
// table lookup of its own. The sysret label stays `.hidden` -- it is this
// image's own business, and an exported symbol is a promise to something
// outside it. The kernel reads the return address out of the syscall frame
// and kills a process whose `syscall` (the two bytes that end there) is not
// inside this image's mapped range; that address is the label's.
//
// Registers. The arguments arrive in the SysV argument registers and the
// kernel reads them from %rdi, %rsi, %rdx, %r10, %r8, %r9, %r12, %r13 -- the
// SysV order for the first six, which is what lets a short stub be the trap
// and nothing else. %eax carries the number in and the Status out.
//
//   - %rcx and %r11 can never be argument registers: the SYSCALL instruction
//     overwrites both, with the return address and RFLAGS, before the kernel
//     sees them. So a fourth argument, which SysV passes in %rcx, is moved to
//     %r10 first -- the register the kernel reads it from.
//
//   - SysV puts the seventh and eighth arguments on the caller's stack, just
//     above the return address, and the kernel reads them from %r12 and %r13
//     (the syscall frame saves both). Those two are callee-saved, so the stub
//     owes its caller their old values: it pushes them, loads the arguments
//     into them, traps, and restores them.
//
//       8 arguments, after both pushes:   7 arguments, after one push:
//          0(%rsp)  saved %r13               0(%rsp)  saved %r12
//          8(%rsp)  saved %r12               8(%rsp)  return addr
//         16(%rsp)  return addr             16(%rsp)  argument 7
//         24(%rsp)  argument 7
//         32(%rsp)  argument 8
//
//     Seven saves and loads %r12 alone: %r13 carries nothing, and the kernel
//     neither reads it nor lets it change, so the stub owes nothing for it.
//     vma_map is the call with seven. More than eight is refused outright.
//
// A 32-bit argument -- a handle, a count, an option word -- is left as it is:
// SysV leaves the upper half of its register unspecified, and the kernel
// truncates rather than trusting it to be zero, so there is nothing to clear.
//
// A noreturn call ends in ud2 rather than ret: returning from it would be a
// kernel bug, and a fault at the next instruction says so where a `ret` would
// wander off into whatever the caller had on its stack.
//
// A blocking call that comes back with InternalIrqRetry traps again, with the
// same arguments, and the program never sees that status. The kernel answers
// it when a suspend ends the call's wait: the thread stops at the syscall
// exit, and once it is resumed the call is simply made again. Two things make
// the plain jump back correct:
//
//   - The kernel restores every register it read an argument from: the exit
//     path pops the whole syscall frame, and a call writes only the result
//     into it. So %rdi..%r9, %r10, %r12 and %r13 hold what the first trap
//     read, and the retry starts after the moves that put them there -- after
//     the pushes too, so the stack is as the first trap left it.
//   - A blocking call is one that can be made again from the start: an
//     interrupted wait has changed nothing, or only what doing it again
//     repeats harmlessly (system/public/ours/syscalls.gni, attrs). A call that
//     changes something and then waits cannot be blocking, and has to be split
//     as a call and a finish instead.
//
// The retry goes back to the same `syscall` instruction, so the address the
// kernel checks is the same on every trap.
//
// The status's value is part of this code. It is pinned against
// Status::InternalIrqRetry in system/public/ours/status.hpp.
#define OURS_STATUS_INTERNAL_IRQ_RETRY 31

.macro ours_syscall name, sysret, number, nargs, noreturn, blocking
.balign 16
.globl \name
.type \name, @function
\name:
.if \nargs <= 3
.elseif \nargs <= 6
    mov %rcx, %r10
.elseif \nargs == 7
    push %r12
    mov %rcx, %r10
    mov 16(%rsp), %r12
.elseif \nargs == 8
    push %r12
    push %r13
    mov %rcx, %r10
    mov 24(%rsp), %r12
    mov 32(%rsp), %r13
.else
    .error "a vDSO stub takes 0 to 8 arguments"
.endif
.if \blocking
.Lretry_\name:
.endif
    mov $\number, %eax
    syscall
.globl \sysret
.hidden \sysret
\sysret:
.if \noreturn
    ud2
.else
.if \blocking
    cmp $OURS_STATUS_INTERNAL_IRQ_RETRY, %eax
    je .Lretry_\name
.endif
.if \nargs == 8
    pop %r13
    pop %r12
.elseif \nargs == 7
    pop %r12
.endif
    ret
.endif
.size \name, . - \name
.endm

.section .text, "ax", @progbits

#include <syscall_abi/vdso_stubs.inc>

// The monotonic anchor. Read-only in the image so the vDSO has no writable
// segment; the kernel writes the same bytes through the VMO. The compiler
// never sees an initializer, so a read is a read. ours_time_values is hidden:
// a program does not bind to it, and the kernel finds it by the magic.
.section .rodata, "a", @progbits
.balign 64
.global ours_time_values
.hidden ours_time_values
.type ours_time_values, @object
ours_time_values:
    .quad 0x0C10C1A11E000001
    .quad 0
    .quad 0
    .quad 0
    .long 0
    .long 1
    .byte 0
    .zero 7
    .quad 0
.size ours_time_values, . - ours_time_values

.section .note.GNU-stack, "", @progbits
