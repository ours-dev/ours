// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_HALT_HPP
#define ARCH_RISCV_HALT_HPP 1

// Only ever reached via <arch/halt.hpp>'s own #include — don't include this
// file directly.

namespace arch {
inline auto halt() noexcept -> void {
    asm volatile("wfi" ::: "memory");
}
} // namespace arch

#endif // ARCH_RISCV_HALT_HPP
