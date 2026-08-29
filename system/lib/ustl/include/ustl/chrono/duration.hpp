/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_CHRONO_DURATION_HPP
#define USTL_CHRONO_DURATION_HPP 1

#include <chrono>

#include <cstdint>

namespace ustl::chrono {

using Femtoseconds = std::chrono::duration<std::int64_t, std::femto>;
using Picoseconds = std::chrono::duration<std::int64_t, std::pico>;
using Nanoseconds = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using Minutes = std::chrono::minutes;
using Years = std::chrono::years;

template <typename ClockT, typename DurationT>
struct TimePoint: std::chrono::time_point<ClockT, DurationT> {
    using Base = std::chrono::time_point<ClockT, DurationT>;
    using Duration = DurationT;

    constexpr TimePoint() = default;
    constexpr TimePoint(Base const &base) : Base(base) {}
    using Base::Base;
    using Base::operator+=;
    using Base::operator-=;
    using Base::operator=;
};

using std::chrono::duration_cast;
using std::chrono::time_point_cast;

} // namespace ustl::chrono

#endif // USTL_CHRONO_DURATION_HPP
