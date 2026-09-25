// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
//
// The one `syscall` instruction in this tree outside the vDSO -- on purpose.
//
// The rule is that a user program enters the kernel only through the vDSO:
// the kernel refuses, and since P1-A kills the process for, a trap whose
// return address is not inside the vDSO (kernel/lib/syscall/src/table.cpp,
// trapped_from_vdso()). A rule the kernel enforces needs a program that
// breaks it, or nothing shows the enforcement still works. This is that
// program's one instruction: part of lib/syscall's test, packaged only into
// the diagnostics image and run only as the trap_outside_vdso probe. Nothing
// else may copy it.
//
// void fault_probe_trap_outside_vdso()
//
// Call number 0 in %eax, as the vDSO's debug_write stub would load it; the
// number does not matter, because the kernel decides before it looks at it.
// If the kernel ever lets this trap through, the call returns and the probe
// reports that it survived.

.section .text.fault_probe_trap_outside_vdso, "ax", @progbits
.balign 16
.globl fault_probe_trap_outside_vdso
.type fault_probe_trap_outside_vdso, @function
fault_probe_trap_outside_vdso:
    xorl %eax, %eax
    syscall
    ret
.size fault_probe_trap_outside_vdso, . - fault_probe_trap_outside_vdso

.section .note.GNU-stack, "", @progbits
