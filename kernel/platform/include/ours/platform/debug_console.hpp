/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_DEBUG_CONSOLE_HPP
#define OURS_PLATFORM_DEBUG_CONSOLE_HPP

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::platform {
/// Blocks for one serial debug byte, then copies up to |capacity| available
/// bytes into the kernel buffer. A zero capacity returns immediately.
auto debug_console_read(char *buffer, usize capacity, usize *actual) -> Status;

/// Writes the |n| bytes at |s| to the debug console, waiting on the device
/// for each one by polling, and returns once the last has been handed over.
///
/// The kernel's console logger is built on it (kernel/lib/logz4), from the
/// first line of the boot to the last line of a panic, so it may need nothing
/// that is set up later: no memory, no CPU-local storage, no interrupt, no
/// lock. It takes no lock and leaves interrupts as it finds them; keeping one
/// writer on the device at a time is the caller's job.
auto debug_console_write(char const *s, usize n) -> void;
} // namespace ours::platform

#endif // OURS_PLATFORM_DEBUG_CONSOLE_HPP
