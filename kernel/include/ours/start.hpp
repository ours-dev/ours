/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_START_HPP
#define OURS_START_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>

namespace ours {
extern "C" auto start_kernel(PhysAddr handoff) -> void;

extern "C" [[noreturn]]
auto start_nonboot_cpu() -> void;

/// Installs the bootstrap console logger; defined by kernel/lib/logz4,
/// empty on architectures without a known bootstrap port. Must run before any
/// subsystem logs, at the very top of start_kernel().
auto init_bootstrap_logging() -> void;

/// Defined by arch-specific code (e.g. kernel/arch/x86/kernel/init.cpp).
extern "C" auto init_arch_early() -> void;
extern "C" auto init_arch() -> void;
} // namespace ours

#endif // OURS_START_HPP
