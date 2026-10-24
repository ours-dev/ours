/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_MACHINE_HPP
#define ARCH_X86_MACHINE_HPP 1

// Only ever reached via <arch/machine.hpp>'s own #include — don't include
// this file directly.

namespace arch {

inline auto machine_kind() -> MachineKind {
    u32 eax = 1;
    u32 ebx;
    u32 ecx;
    u32 edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

    // CPUID.1:ECX bit 31 is left to hypervisors: a processor always reports
    // it clear, and anything presenting a virtual one sets it. An emulator
    // that leaves it clear is taken for bare metal; nothing in CPUID tells
    // the two apart then.
    if ((ecx & (u32(1) << 31)) == 0) {
        return MachineKind::BareMetal;
    }

    // With the bit set, leaf 0x4000'0000 is the hypervisor's own: EBX, ECX
    // and EDX spell its vendor in twelve bytes. QEMU answers "TCGTCGTCGTCG"
    // when its translator runs the code, and another vendor ("KVMKVMKVM",
    // "Microsoft Hv", ...) when the processor does.
    eax = 0x4000'0000;
    u32 signature[3];
    asm volatile("cpuid" : "+a"(eax), "=b"(signature[0]), "=c"(signature[1]), "=d"(signature[2]));
    if (__builtin_memcmp(signature, "TCGTCGTCGTCG", sizeof(signature)) == 0) {
        return MachineKind::Emulated;
    }
    return MachineKind::Virtualized;
}

} // namespace arch

#endif // ARCH_X86_MACHINE_HPP
