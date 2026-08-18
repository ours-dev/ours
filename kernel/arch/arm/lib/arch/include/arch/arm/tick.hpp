/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_ARM_TICK_HPP
#define ARCH_ARM_TICK_HPP 1

// Only ever reached via <arch/tick.hpp>'s own #include (Tick's shape is
// already declared there by the time this is processed) — don't include
// this file directly.

namespace arch {

// Physical counter (CNTPCT_EL0) rather than the virtual one (CNTVCT_EL0):
// this kernel is the privileged software itself, not a hypervisor guest, so
// there's no CNTVOFF_EL2 offset to want. Matches x86's TSC in spirit — a
// raw hardware counter with no virtualization indirection.
inline auto Tick::get() -> Tick {
    usize count;
    asm volatile("mrs %0, cntpct_el0" : "=r"(count));
    return {.tsc = count};
}

} // namespace arch

#endif // ARCH_ARM_TICK_HPP
