/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_TIME_HPP
#define OURS_ARCH_X86_TIME_HPP 1

#include <ours/status.hpp>
#include <ours/time/types.hpp>

/// x86's own clock-event calls. time::global_clock_event(), which generic
/// code reads, is declared in <ours/time/clock_event_device.hpp>; its body
/// is in time/time.cpp beside set_global_clock_event().
namespace ours::time {
/// Points the legacy timer line, ISA 0, at `device`. hpet_init() calls it
/// once it has taken IRQ0 from the PIT through the legacy replacement route.
auto set_global_clock_event(ClockEventDevice &device) -> void;

auto hpet_init() -> Status;
auto pit_init() -> Status;

} // namespace ours::time

#endif // #ifndef OURS_ARCH_X86_TIME_HPP
