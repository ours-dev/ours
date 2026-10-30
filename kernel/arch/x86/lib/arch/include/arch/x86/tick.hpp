/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_TICK_HPP
#define ARCH_X86_TICK_HPP 1

// Only ever reached via <arch/tick.hpp>'s own #include (Tick's shape is
// already declared there by the time this is processed) — don't include
// this file directly.

namespace arch {

inline auto Tick::get() -> Tick {
    u32 low;
    u32 high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return {.count = (static_cast<usize>(high) << 32) | low};
}

} // namespace arch

#endif // ARCH_X86_TICK_HPP
