/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_HALT_HPP
#define ARCH_HALT_HPP 1

namespace arch {

/// Stops the current CPU until the next interrupt.
auto halt() noexcept -> void;

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/halt.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/halt.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/halt.hpp>
#endif

#endif // ARCH_HALT_HPP
