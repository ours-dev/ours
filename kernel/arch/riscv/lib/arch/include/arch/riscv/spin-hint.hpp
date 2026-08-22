// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_SPIN_HINT_HPP
#define ARCH_RISCV_SPIN_HINT_HPP 1

// Only ever reached via <arch/spin-hint.hpp>'s own #include — don't include
// this file directly.

namespace arch {
inline auto spin_hint() noexcept -> void {
    // Zihintpause's PAUSE, emitted as a raw word so it assembles without the
    // extension enabled.  The encoding is a FENCE hint, which harts that do
    // not implement Zihintpause are required to retire as a nop.
    asm volatile(".4byte 0x0100000f" ::: "memory");
}
} // namespace arch

#endif // ARCH_RISCV_SPIN_HINT_HPP
