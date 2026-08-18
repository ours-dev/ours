/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RANDOM_HPP
#define ARCH_RANDOM_HPP 1

#include <arch/types.hpp>

namespace arch {

/// Fills `*value` with a hardware-generated random number, returning `false`
/// if the current CPU doesn't support the instruction this needs.
[[nodiscard]] auto rand(usize *value) -> bool;

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/random.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/random.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/random.hpp>
#endif

#endif // ARCH_RANDOM_HPP
