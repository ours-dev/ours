/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CHRONO_HPP
#define OURS_CHRONO_HPP 1

// The kernel's time vocabulary: durations and time points. It used to live
// in system/public as <ours/syscall/time.hpp> -- misfiled: nothing user-side
// includes it, its 18 includers are all kernel-side, and it sat on the
// would-be sysroot path for the no reason. Here it is with the rest of the
// kernel's own headers (kernel/include/ours/), named for what it is.
//
// It is NOT <ours/time.hpp>, which is the clock syscalls' public header
// (system/public/ours/time.hpp): the two live on the kernel toolchain's
// include path together, so the names must not collide.

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

#endif // OURS_CHRONO_HPP
