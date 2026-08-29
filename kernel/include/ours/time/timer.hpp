/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_HPP
#define OURS_TIME_TIMER_HPP 1

#include <ours/mm/cpu_local.hpp>
#include <ours/platform/timer.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/mod.hpp>
#include <ours/time/types.hpp>

#include <atomic>

#include <gktl/canary.hpp>

#include <ustl/collections/intrusive/set.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/function/fn.hpp>

namespace ours::time {
class TimeSlack {
    typedef TimeSlack Self;

  public:
    FORCE_INLINE constexpr TimeSlack() = default;

    template <typename DurationT>
    FORCE_INLINE constexpr TimeSlack(DurationT duration)
        : m_slack(duration_cast<Duration>(duration)) {}

    FORCE_INLINE constexpr auto duration() const -> Duration {
        return m_slack;
    }

  private:
    Duration m_slack;
};

class Deadline {
    using Self = Deadline;

  public:
    FORCE_INLINE constexpr Deadline() = default;

    FORCE_INLINE constexpr Deadline(Duration duration, TimeSlack slack = {})
        : m_time_point(duration), m_time_slack(slack) {}

    template <typename OtherDuration>
    FORCE_INLINE constexpr Deadline(OtherDuration duration, TimeSlack slack = {})
        : m_time_point(duration_cast<Duration>(duration)), m_time_slack(slack) {}

    // template <typename T>
    // FORCE_INLINE constexpr
    // Deadline(Time duration, TimeSlack slack = {})
    // : Self(duration_cast<Nanoseconds>(duration), slack)
    // {}

    FORCE_INLINE constexpr auto when() const -> TimePoint<> {
        return m_time_point;
    }

    FORCE_INLINE constexpr auto earliest() const -> TimePoint<> {
        return m_time_point;
    }

    FORCE_INLINE constexpr auto latest() const -> TimePoint<> {
        return m_time_point + m_time_slack.duration();
    }

    FORCE_INLINE constexpr static auto infinite() -> Self {
        return Self(kInfiniteTime.time_since_epoch());
    }

    FORCE_INLINE constexpr friend auto operator==(Self const &x, Self const &y) -> bool {
        return x.m_time_point == y.m_time_point;
    }

    FORCE_INLINE constexpr friend auto operator!=(Self const &x, Self const &y) -> bool {
        return x.m_time_point != y.m_time_point;
    }

  private:
    TimePoint<> m_time_point;
    TimeSlack m_time_slack;
};

enum class ClockId {
    Monotonic,
    Boottime,
    MaxNumClocks,
};

class Timer: public ustl::collections::intrusive::SetBaseHook<> {
    using Base = ustl::collections::intrusive::SetBaseHook<>;
    using Self = Timer;

  public:
    enum class HandlerResult {
        Handled,
        Restart,
    };
    using Handler = ustl::function::Fn<auto(Timer *)->HandlerResult>;

    constexpr Timer(ClockId clkid = ClockId::Monotonic) : m_canary(this), m_clockid(clkid) {}

    ~Timer() {
        // A timer was destoried before dequeueing.
        DEBUG_ASSERT(!is_linked());
    }

    template <typename F, typename... Args>
    FORCE_INLINE auto activate(Deadline deadline, F &&f, Args &&...args) -> void {
        Handler on_expired =
            ustl::function::bind_front(ustl::forward<F>(f), ustl::forward<Args>(args)...);
        return activate(deadline, on_expired);
    }

    /// Directly activate this timer, meaning that this timer will be added onto per-CPU
    /// TimerScheduler.
    auto activate(Deadline deadline, Handler on_expired) -> void;

    auto forward(Time now, Duration interval) -> void;

    auto forward(Duration interval) -> void;

    auto cancel() -> void;

    auto set_deadline(Deadline expiry) -> void;

    FORCE_INLINE
    auto on_expired() -> HandlerResult {
        DEBUG_ASSERT(m_on_expired);
        return m_on_expired(this);
    }

    FORCE_INLINE
    auto timer_queue() -> TimerQueue * {
        return m_queue;
    }

    FORCE_INLINE
    auto deadline() const -> Deadline {
        return m_deadline;
    }

    FORCE_INLINE
    auto clockid() const -> ClockId {
        return m_clockid;
    }

    FORCE_INLINE
    friend auto operator<(Self const &x, Self const &y) noexcept -> bool {
        return x.m_deadline.latest() < y.m_deadline.latest();
    }

  private:
    friend class TimerQueue;

    gktl::Canary m_canary;
    ClockId m_clockid;
    TimerQueue *m_queue;
    Deadline m_deadline;
    Handler m_on_expired;
    std::atomic<CpuNum> m_active_cpu;
    std::atomic<bool> m_cancalled;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER_HPP