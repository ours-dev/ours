// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_ARM_SPIN_HINT_HPP
#define ARCH_ARM_SPIN_HINT_HPP 1

// Only ever reached via <arch/spin_hint.hpp>'s own #include — don't include
// this file directly.

namespace arch {
inline auto spin_hint() noexcept -> void {
    // YIELD is in the NOP-hint space, so it is architecturally safe even on
    // cores that give it no special meaning.
    asm volatile("yield" ::: "memory");
}
} // namespace arch

#endif // ARCH_ARM_SPIN_HINT_HPP
