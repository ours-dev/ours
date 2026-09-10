// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The vDSO: the only image in a user address space that contains a `syscall`
// instruction.
//
// Zircon counterpart: //zircon/kernel/lib/userabi/vdso. Two things are copied
// from it deliberately. The first is the shape of a stub -- put the number in
// %eax, trap, and label the return address, because that label is what an
// "approved call site" means. The second is using the ELF entry point as the
// handle a program with no dynamic linker can find the exports through:
// Zircon links its vDSO with `-Wl,-e,_zx_process_exit` for exactly that
// reason ("so it can be found without reading the .dynsym").
//
// Written in assembly rather than C++, and not only because of the `syscall`
// instruction. The entry table below holds offsets computed by the linker
// (`entry - OURS_VDSO_TABLE`); C++ cannot form that as a constant initialiser,
// so a C++ table would have to be filled in at run time by code that does not
// exist here -- there is no startup in a vDSO.

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

// ---- The entry table -------------------------------------------------------
//
// Placed at the ELF entry point by -Wl,-e,OURS_VDSO_TABLE (see BUILD.gn), so
// the kernel finds it with the field it already reads to load any image. Every
// entry is a byte offset from the start of this table: nothing relocates the
// vDSO, so an absolute address here would be whatever the linker guessed.

.section .rodata.vdso.table, "a", @progbits
.balign 8
.globl OURS_VDSO_TABLE
.hidden OURS_VDSO_TABLE
OURS_VDSO_TABLE:
    .long OURS_VDSO_MAGIC
    .short OURS_VDSO_VERSION
    .short OURS_VDSO_NR_ENTRIES
    .long ours_vdso_debug_write - OURS_VDSO_TABLE
    .long ours_vdso_command_list - OURS_VDSO_TABLE
.size OURS_VDSO_TABLE, . - OURS_VDSO_TABLE

// ---- The stubs -------------------------------------------------------------

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
.globl ours_vdso_debug_write
.hidden ours_vdso_debug_write
.type ours_vdso_debug_write, @function
ours_vdso_debug_write:
    mov $OURS_SYS_DEBUG_WRITE, %eax
    syscall
    // The kernel reads the return address out of the syscall frame and refuses
    // any trap whose return address is outside this image. That address is the
    // one right here, one byte past `syscall`.
.globl OURS_VDSO_SYSRET_DEBUG_WRITE
.hidden OURS_VDSO_SYSRET_DEBUG_WRITE
OURS_VDSO_SYSRET_DEBUG_WRITE:
    ret
.size ours_vdso_debug_write, . - ours_vdso_debug_write

// ours_command_list(char *buffer, usize length) -> Status
//
// Same shape as the stub above, and deliberately so: a stub that did anything
// besides move the number into %eax and trap would be a place for the ABI to
// drift between the two.
.balign 16
.globl ours_vdso_command_list
.hidden ours_vdso_command_list
.type ours_vdso_command_list, @function
ours_vdso_command_list:
    mov $OURS_SYS_COMMAND_LIST, %eax
    syscall
.globl OURS_VDSO_SYSRET_COMMAND_LIST
.hidden OURS_VDSO_SYSRET_COMMAND_LIST
OURS_VDSO_SYSRET_COMMAND_LIST:
    ret
.size ours_vdso_command_list, . - ours_vdso_command_list

.section .note.GNU-stack, "", @progbits
