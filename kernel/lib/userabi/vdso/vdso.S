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

// ours_handle_close(HandleValue handle) -> Status
//
// One argument, already in %edi. The kernel reads it as a u32 and ignores the
// upper half of %rdi, which the SysV ABI leaves unspecified for a 32-bit
// argument -- so there is nothing to clear here either.
.balign 16
.globl ours_handle_close
.type ours_handle_close, @function
ours_handle_close:
    mov $OURS_SYS_HANDLE_CLOSE, %eax
    syscall
.globl OURS_VDSO_SYSRET_HANDLE_CLOSE
.hidden OURS_VDSO_SYSRET_HANDLE_CLOSE
OURS_VDSO_SYSRET_HANDLE_CLOSE:
    ret
.size ours_handle_close, . - ours_handle_close

// ours_channel_create(ChannelCreateOptions options, HandleValue *out0,
//                     HandleValue *out1) -> Status
//
// Three arguments, all in their SysV registers already.
.balign 16
.globl ours_channel_create
.type ours_channel_create, @function
ours_channel_create:
    mov $OURS_SYS_CHANNEL_CREATE, %eax
    syscall
.globl OURS_VDSO_SYSRET_CHANNEL_CREATE
.hidden OURS_VDSO_SYSRET_CHANNEL_CREATE
OURS_VDSO_SYSRET_CHANNEL_CREATE:
    ret
.size ours_channel_create, . - ours_channel_create

// ours_channel_write(HandleValue channel, ChannelWriteOptions options,
//                    void const *bytes, u32 num_bytes,
//                    HandleValue const *handles, u32 num_handles) -> Status
//
// The first stub with a fourth argument. SysV passes it in %rcx, which the
// SYSCALL instruction is about to overwrite with the return address, so it
// moves to %r10 -- the register the kernel reads it from. %r8 and %r9 are
// already where the kernel expects them.
.balign 16
.globl ours_channel_write
.type ours_channel_write, @function
ours_channel_write:
    mov %rcx, %r10
    mov $OURS_SYS_CHANNEL_WRITE, %eax
    syscall
.globl OURS_VDSO_SYSRET_CHANNEL_WRITE
.hidden OURS_VDSO_SYSRET_CHANNEL_WRITE
OURS_VDSO_SYSRET_CHANNEL_WRITE:
    ret
.size ours_channel_write, . - ours_channel_write

// ours_channel_read(HandleValue channel, ChannelReadOptions options,
//                   void *bytes, HandleValue *handles, u32 num_bytes,
//                   u32 num_handles, u32 *actual_bytes,
//                   u32 *actual_handles) -> Status
//
// Eight arguments. The first six arrive in registers; SysV puts the seventh
// and eighth on the caller's stack, just above the return address, and the
// kernel reads them from %r12 and %r13 (the frame saves both). Those two are
// callee-saved, so this stub owes its caller their old values: it pushes
// them, loads the arguments into them, traps, and restores them.
//
// Stack on entry:           after both pushes:
//    0(%rsp)  return addr      0(%rsp)  saved %r13
//    8(%rsp)  actual_bytes     8(%rsp)  saved %r12
//   16(%rsp)  actual_handles  16(%rsp)  return addr
//                             24(%rsp)  actual_bytes
//                             32(%rsp)  actual_handles
//
// Zircon's vDSO does exactly this, at the same offsets (0x18 and 0x20):
// the m_syscall macro in kernel/lib/userabi/vdso/syscalls-x86.S.
.balign 16
.globl ours_channel_read
.type ours_channel_read, @function
ours_channel_read:
    push %r12
    push %r13
    mov %rcx, %r10
    mov 24(%rsp), %r12
    mov 32(%rsp), %r13
    mov $OURS_SYS_CHANNEL_READ, %eax
    syscall
.globl OURS_VDSO_SYSRET_CHANNEL_READ
.hidden OURS_VDSO_SYSRET_CHANNEL_READ
OURS_VDSO_SYSRET_CHANNEL_READ:
    pop %r13
    pop %r12
    ret
.size ours_channel_read, . - ours_channel_read

// ours_object_wait_one(HandleValue handle, Signals signals, Instant deadline,
//                      Signals *observed) -> Status
//
// Four arguments: the fourth moves from %rcx to %r10, as in channel_write.
.balign 16
.globl ours_object_wait_one
.type ours_object_wait_one, @function
ours_object_wait_one:
    mov %rcx, %r10
    mov $OURS_SYS_OBJECT_WAIT_ONE, %eax
    syscall
.globl OURS_VDSO_SYSRET_OBJECT_WAIT_ONE
.hidden OURS_VDSO_SYSRET_OBJECT_WAIT_ONE
OURS_VDSO_SYSRET_OBJECT_WAIT_ONE:
    ret
.size ours_object_wait_one, . - ours_object_wait_one

// ours_process_spawn_boot(HandleValue job, char const *name, usize name_size,
//                         HandleValue bootstrap) -> Status
//
// TEMPORARY (see <ours/vdso.hpp>). Four arguments: the fourth moves from %rcx
// to %r10.
.balign 16
.globl ours_process_spawn_boot
.type ours_process_spawn_boot, @function
ours_process_spawn_boot:
    mov %rcx, %r10
    mov $OURS_SYS_PROCESS_SPAWN_BOOT, %eax
    syscall
.globl OURS_VDSO_SYSRET_PROCESS_SPAWN_BOOT
.hidden OURS_VDSO_SYSRET_PROCESS_SPAWN_BOOT
OURS_VDSO_SYSRET_PROCESS_SPAWN_BOOT:
    ret
.size ours_process_spawn_boot, . - ours_process_spawn_boot

.section .note.GNU-stack, "", @progbits
