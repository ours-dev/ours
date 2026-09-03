/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_TIME_HPP
#define OURS_ARCH_TIME_HPP 1

#include <ours/status.hpp>
#include <ours/time/types.hpp>

namespace ours::time {
extern ClockEventDevice *CLOCK_EVENT;

inline auto global_clock_event() -> ClockEventDevice * {
    return CLOCK_EVENT;
}

auto hpet_init() -> Status;
auto pit_init() -> Status;

} // namespace ours::time

#endif // #ifndef OURS_ARCH_TIME_HPP
