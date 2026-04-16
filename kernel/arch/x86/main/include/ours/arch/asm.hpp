/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_ASM_HPP
#define OURS_ARCH_ASM_HPP 1

#include <arch/asm.hpp>

#define KERNEL_LOAD_ADDR    0x0000000000100000
#define KERNEL_BASE         0xffffff8000100000

#define EALY_IDT_ENTRIES    32
#define PHYS(x) ((x) - (KERNEL_BASE - KERNEL_LOAD_ADDR))

#endif // #ifndef OURS_ARCH_ASM_HPP