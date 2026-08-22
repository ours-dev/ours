// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_SPIN_HINT_HPP
#define ARCH_X86_SPIN_HINT_HPP 1

// Only ever reached via <arch/spin-hint.hpp>'s own #include — don't include
// this file directly.

namespace arch {
inline auto spin_hint() noexcept -> void {
    // PAUSE. Encoded as REP NOP, so it degrades to a plain nop on pre-SSE2
    // parts rather than faulting.
    asm volatile("pause" ::: "memory");
}
} // namespace arch

#endif // ARCH_X86_SPIN_HINT_HPP
