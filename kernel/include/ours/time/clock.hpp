/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_HPP
#define OURS_TIME_CLOCK_HPP 1

#include <gktl/canary.hpp>
#include <ours/status.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/clock_type.hpp>
#include <ours/time/types.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::time {
class Clock {
    using Self = Clock;

  public:
    using TimePoint = TimePoint<Self>;

    Clock();

    auto now() -> TimePoint {
        update_time();
        return m_time_base + m_time_shift;
    }

    /// The clock source's raw counter, with none of the scaling now()
    /// applies. Zero before a source has been registered, which is the
    /// only sensible answer when there is nothing to read.
    auto ticks() -> Ticks;

    auto change_source(ClockSource *source) -> Status;

    /// Forcely override the original clock source, recommended to
    /// use change_source() instead of it;
    auto override_source(ClockSource *source) -> Status;

  private:
    auto update_time() -> void;

    gktl::Canary m_canary;
    u32 numerator_;
    u32 denominator_;
    usize m_cycle_last;
    Duration m_time_shift;
    TimePoint m_time_base;
    ClockSource *m_clocksource;
    ustl::sync::AtomicU8 m_seq;
};

class SystemClock {
    using Self = SystemClock;

  public:
    using TimePoint = TimePoint<Self>;

    static auto now() -> TimePoint;

    static auto ticks() -> Ticks;

    // For changes on ClockSource is able to notify system clock.
    static auto notify(ClockSource *source) -> Status;

  private:
    static inline Clock s_clock;
    static inline TimePoint s_real_base;
};

class SteadyClock {
    using Self = SteadyClock;

  public:
    using TimePoint = TimePoint<Self>;

    static auto now() -> TimePoint;

    static auto ticks() -> Ticks;

  private:
    static inline Clock s_clock;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_HPP