/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_TIME_HPP
#define OURS_ARCH_TIME_HPP 1

#include <ours/status.hpp>
#include <ours/time/types.hpp>

namespace ours::time {
/// The clock-event device the legacy timer line, ISA 0, drives: the PIT's
/// until the HPET takes the line over.
auto global_clock_event() -> ClockEventDevice *;

/// Points the legacy timer line at `device`. hpet_init() calls it once it
/// has taken IRQ0 from the PIT through the legacy replacement route.
auto set_global_clock_event(ClockEventDevice &device) -> void;

auto hpet_init() -> Status;
auto pit_init() -> Status;

} // namespace ours::time

#endif // #ifndef OURS_ARCH_TIME_HPP
