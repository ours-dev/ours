/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_TICK_HPP
#define ARCH_RISCV_TICK_HPP 1

// Only ever reached via <arch/tick.hpp>'s own #include (Tick's shape is
// already declared there by the time this is processed) — don't include
// this file directly.

namespace arch {

inline auto Tick::get() -> Tick {
    usize count;
    // `rdtime` is the assembler's standard pseudo-instruction for reading
    // the `time` CSR (delegated to S-mode by SBI/M-mode firmware on real
    // systems).
    asm volatile("rdtime %0" : "=r"(count));
    return {.count = count};
}

} // namespace arch

#endif // ARCH_RISCV_TICK_HPP
