/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_START_HPP
#define OURS_START_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>

namespace ours {
    NO_MANGLE auto start_kernel(PhysAddr handoff) -> void;

    NO_MANGLE [[noreturn]] auto start_nonboot_cpu() -> void;

    /// Defined by arch-specific code (e.g. kernel/arch/x86/kernel/init.cpp).
    NO_MANGLE auto init_arch_early() -> void;
    NO_MANGLE auto init_arch() -> void;
} // namespace ours

#endif // OURS_START_HPP
