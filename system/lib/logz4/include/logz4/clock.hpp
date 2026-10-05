/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_CLOCK_HPP
#define LOGZ4_CLOCK_HPP 1

#include <ours/types.hpp>

namespace logz4 {

/// When a line happened. The unit is the clock's: logz4 never converts
/// it (no Hz, no seconds). It stores a time quantity and leaves the
/// epoch/unit to whoever supplied the clock.
using Time = u64;

/// Plug in the log clock. `read` may be null, which means "no time on
/// the line" — phys and tests that have not installed a clock yet.
auto set_clock(Time (*read)()) -> void;

[[nodiscard]]
auto clock() -> Time (*)();

/// Which CPU wrote the line. Independent of the clock; may stay unset
/// until CPU-local storage exists.
auto set_cpu(u32 (*read)()) -> void;

[[nodiscard]]
auto cpu() -> u32 (*)();

} // namespace logz4

#endif // LOGZ4_CLOCK_HPP
