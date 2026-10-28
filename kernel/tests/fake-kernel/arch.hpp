/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What the fake kernel needs from the machine under it: a byte out to the
/// console, a way to stop, and whether the handoff names the console it
/// writes to.  arch_<cpu>.cpp defines them for each architecture, and
/// BUILD.gn compiles the one for the target, as it picks entry_<cpu>.S.
#ifndef OURS_TESTS_FAKE_KERNEL_ARCH_HPP
#define OURS_TESTS_FAKE_KERNEL_ARCH_HPP 1

#include <ours/phys/handoff.hpp>

namespace ours::fake_kernel {
/// Writes one byte to the console, once the device can take it.
auto write_char(char const character) -> void;

/// Stops this CPU for good, with interrupts masked.
[[noreturn]]
auto halt() -> void;

/// Whether `uart`, the debug UART physload recorded in the handoff, is the
/// console write_char() writes to: a report on whether the handoff plumbing
/// carried it, rather than this image's own assumption about the console
/// happening to work regardless.
[[nodiscard]]
auto handoff_names_console(phys::UartHandoff const &uart) -> bool;
} // namespace ours::fake_kernel

#endif // OURS_TESTS_FAKE_KERNEL_ARCH_HPP
