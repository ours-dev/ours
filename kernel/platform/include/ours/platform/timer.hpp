/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_TIMER_HPP
#define OURS_PLATFORM_TIMER_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
auto current_mono_ticks() -> Ticks;

auto current_mono_time() -> Time;

auto platform_convert_mono_time_to_ticks(Time timepoint) -> Ticks;

auto platform_set_oneshot_timer(Ticks deadline) -> Status;

/// Unit: ms
auto get_periodic_time() -> usize;

/// The rate at which arch::Tick::get() counts, in counts per second, or 0
/// while the platform does not know it yet. Once it answers non-zero the
/// answer does not change.
///
/// It is what turns a span measured in arch::Tick counts into a time. A
/// caller that may run before the rate is known -- a panic in early boot --
/// has to have an answer for 0.
[[nodiscard]]
auto arch_tick_hz() -> u64;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_TIMER_HPP