/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_BOOT_MARKER_HPP
#define ARCH_RISCV_BOOT_MARKER_HPP 1

#include <arch/riscv/sbi.hpp>

namespace arch {
/// The firmware's console is the one port that needs no setup here: the
/// legacy SBI putchar works from the first instruction S-mode runs.
inline auto boot_marker(char const c) -> void {
    sbi::console_putchar(c);
}
} // namespace arch

#endif // ARCH_RISCV_BOOT_MARKER_HPP
