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
} // namespace ours::platform

#endif // OURS_PLATFORM_DEBUG_CONSOLE_HPP