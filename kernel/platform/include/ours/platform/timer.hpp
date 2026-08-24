/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_TIMER_HPP
#define OURS_PLATFORM_TIMER_HPP 1

#include <ours/types.hpp>
#include <ours/status.hpp>

namespace ours {
    auto current_mono_ticks() -> Ticks;

    auto current_mono_time() -> Time;

    auto platform_convert_mono_time_to_ticks(Time timepoint) -> Ticks;

    auto platform_set_oneshot_timer(Ticks deadline) -> Status;

    /// Unit: ms
    auto get_periodic_time() -> usize;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_TIMER_HPP