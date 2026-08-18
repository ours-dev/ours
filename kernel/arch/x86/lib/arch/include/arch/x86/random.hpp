/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_RANDOM_HPP
#define ARCH_X86_RANDOM_HPP 1

// Only ever reached via <arch/random.hpp>'s own #include — don't include
// this file directly.

namespace arch {

inline auto rand(usize *value) -> bool {
    u32 eax = 1;
    u32 ebx;
    u32 ecx;
    u32 edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if ((ecx & (u32(1) << 30)) == 0) {
        return false;
    }

    unsigned char generated;
    asm volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(generated));
    return generated != 0;
}

} // namespace arch

#endif // ARCH_X86_RANDOM_HPP
