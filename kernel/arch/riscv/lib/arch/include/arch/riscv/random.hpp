/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_RANDOM_HPP
#define ARCH_RISCV_RANDOM_HPP 1

// Only ever reached via <arch/random.hpp>'s own #include — don't include
// this file directly.

namespace arch {

// The base RISC-V ISA has no hardware RNG instruction; a true-entropy source
// only exists behind the optional Zkr extension (the `seed` CSR), which
// isn't in this project's -march baseline (see
// kernel/arch/riscv/BUILD.gn:abi.integer-register). Always report
// unavailable rather than guess at an extension that may not be present.
inline auto rand(usize *) -> bool {
    return false;
}

} // namespace arch

#endif // ARCH_RISCV_RANDOM_HPP
