/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_TIME_HPP
#define OURS_SYSCALL_TIME_HPP 1

#include <ours/types.hpp>

#include <ustl/chrono/duration.hpp>
#include <ustl/limits.hpp>

namespace ours::time {
class SteadyClock;
class SystemClock;
} // namespace ours::time

namespace ours {

using ustl::chrono::Femtoseconds;
using ustl::chrono::Microseconds;
using ustl::chrono::Milliseconds;
using ustl::chrono::Minutes;
using ustl::chrono::Nanoseconds;
using ustl::chrono::Picoseconds;
using ustl::chrono::Seconds;
using ustl::chrono::Years;

using Duration = Nanoseconds;

template <typename Clock = time::SteadyClock>
using TimePoint = ustl::chrono::TimePoint<Clock, Duration>;

using ustl::chrono::duration_cast;
using ustl::chrono::time_point_cast;

inline constexpr auto INFINITE_TIME = TimePoint<>::max();
inline constexpr auto INFINITE_TICKS = ustl::NumericLimits<Ticks>::max();

constexpr auto operator""_s(unsigned long long seconds) -> Seconds {
    return Seconds(static_cast<Seconds::rep>(seconds));
}

} // namespace ours

#endif // OURS_SYSCALL_TIME_HPP
