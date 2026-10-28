// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// Entry of the riscv boot shim.
//
// The firmware (OpenSBI) enters the image in S-mode with the MMU off, a0 the
// boot hart's ID and a1 the physical address of the device tree. Nothing else
// is promised: the stack pointer, the trap vector and the interrupt enables
// are whatever the firmware left.
//
// The image starts with the Linux boot image header, so a loader that takes a
// flat binary (and reads text_offset and the magic numbers) accepts it as it
// would a Linux kernel. QEMU loading an ELF ignores the header and jumps to
// the ELF entry, PHYS_ENTRY.

.section .boot.header, "ax", @progbits

// code0 and code1 are the two words a loader may execute first; the jump
// skips the rest of the header.
.global _start
_start:
    j       .Lreal_start
    .word   0                           // code1, unused

    // Where from the start of RAM the loader should put the image. The first
    // 2 MiB of RAM belong to the firmware, so the image sits just above it.
    .dword  0x200000                    // text_offset

    // Bytes the image occupies in memory, bss included, so the loader leaves
    // room for it.
    .dword  __executable_end - __executable_start // image_size

    .dword  0                           // flags: little-endian
    .word   0x2                         // version 0.2
    .word   0                           // reserved
    .dword  0                           // reserved
    .ascii  "RISCV\0\0\0"               // magic
    .ascii  "RSC\x05"                   // magic2
    .word   0                           // reserved (PE/COFF header offset)

.section .boot.text, "ax", @progbits
.Lreal_start:
    // The firmware may have left interrupts on or a trap vector pointing at
    // its own memory; neither is of any use here. Mask every interrupt, point
    // the vector at nothing, and make sure address translation is off.
    csrc    sstatus, 0x2                // SSTATUS.SIE
    csrw    sie, zero
    csrw    stvec, zero
    csrw    satp, zero

    // a0 and a1 carry the boot parameters into bootshim_main, so the loop
    // below must not touch them. It stores bytes: the linker script aligns
    // the start of .bss but not its end, and a word store at the tail would
    // write past the image.
    lla     t0, __bss_start
    lla     t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sb      zero, 0(t0)
    addi    t0, t0, 1
    j       1b
2:
    lla     sp, BOOT_STACK_END
    mv      ra, zero
    mv      s0, zero

    call    bootshim_main

    // bootshim_main does not return. If it ever does, stop here rather than
    // run into whatever follows.
3:
    wfi
    j       3b

.global PHYS_ENTRY
PHYS_ENTRY = _start

// physload, physboot and the kernel's first instructions all run as nested
// calls on this one stack: nothing downstream switches stacks before the
// kernel does. 256 KiB, the size x86's boot stack had to grow to after 16 KiB
// overflowed under the module loader. It lies in .bss, so the reservation of
// the shim's image keeps it out of the memory physload hands out.
.section .bss.boot, "aw", @nobits
.balign 16
BOOT_STACK:
    .skip   4096 * 64
.global BOOT_STACK_END
BOOT_STACK_END:

.section .note.GNU-stack, "", @progbits
