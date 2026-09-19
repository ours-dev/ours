// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The vDSO: the only image in a user address space that contains a `syscall`
// instruction.
//
// Zircon counterpart: //zircon/kernel/lib/userabi/vdso, from which the shape
// of a stub is copied deliberately -- put the number in %eax, trap, and label
// the return address, because that label is what an "approved call site"
// means.
//
// This file used to carry an export table at the ELF entry point as well, so
// that a program with no dynamic linker could find the stubs by offset. It is
// gone: the image is linked -shared now, so the stubs are in .dynsym, and the
// kernel binds a program's imports to them as it loads the program
// (kernel/lib/userabi/src/userboot.cpp). The ELF format's own answer, in place
// of a private one.
//
// Written in assembly rather than C++ because of the `syscall` instruction and
// because the return-address labels have to be exact: a stub that did anything
// besides move the number into %eax and trap would be a place for the ABI to
// drift.

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

// ---- The stubs -------------------------------------------------------------
//
// Each stub is `.globl` with default visibility, which is what puts it in
// .dynsym: this object is linked -shared, and .dynsym is how a program that
// merely *calls* ours_debug_write() gets bound to it at load time, with no
// table lookup of its own. The OURS_VDSO_SYSRET_* return addresses stay
// `.hidden` -- they are this image's own business, and an exported symbol is a
// promise to something outside it.

.section .text, "ax", @progbits

// ours_debug_write(char const *buffer, usize length) -> Status
//
// The arguments arrive in %rdi and %rsi under the ordinary SysV convention and
// stay there: the syscall ABI was chosen to match it for the first six
// arguments precisely so a stub is the trap and nothing else. %eax carries the
// number in and the Status out.
//
// %rcx and %r11 are clobbered by the SYSCALL instruction itself, which is why
// they can never be argument registers -- the fourth SysV argument register is
// %r10 on this path, not %rcx. This stub takes two arguments, so it does not
// have to move anything.
.balign 16
.globl ours_debug_write
.type ours_debug_write, @function
ours_debug_write:
    mov $OURS_SYS_DEBUG_WRITE, %eax
    syscall
    // The kernel reads the return address out of the syscall frame and refuses
    // any trap whose return address is outside this image. That address is the
    // one right here, one byte past `syscall`.
.globl OURS_VDSO_SYSRET_DEBUG_WRITE
.hidden OURS_VDSO_SYSRET_DEBUG_WRITE
OURS_VDSO_SYSRET_DEBUG_WRITE:
    ret
.size ours_debug_write, . - ours_debug_write

// ours_command_list(char *buffer, usize length) -> Status
//
// Same shape as the stub above, and deliberately so: a stub that did anything
// besides move the number into %eax and trap would be a place for the ABI to
// drift between the two.
.balign 16
.globl ours_command_list
.type ours_command_list, @function
ours_command_list:
    mov $OURS_SYS_COMMAND_LIST, %eax
    syscall
.globl OURS_VDSO_SYSRET_COMMAND_LIST
.hidden OURS_VDSO_SYSRET_COMMAND_LIST
OURS_VDSO_SYSRET_COMMAND_LIST:
    ret
.size ours_command_list, . - ours_command_list

.section .note.GNU-stack, "", @progbits
