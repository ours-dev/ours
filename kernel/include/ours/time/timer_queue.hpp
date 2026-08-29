/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_QUEUE_HPP
#define OURS_TIME_TIMER_QUEUE_HPP 1

#include <ours/syscall/time.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/timer.hpp>

namespace ours::time {
/// The TimerQueue is a per-Clock queue that manages timer events.
class TimerQueue {
    using Self = TimerQueue;

  public:
    /// Attempt to run those expired timers on this queue.
    ///
    /// Return how many timer expired.
    auto run_expired_timers(CpuNum cpu) -> usize;

  private:
    friend class Timer;

    auto update_next_deadline() -> void;

    auto insert_timer(Timer &timer, TimePoint<> earliest, TimePoint<> latest) -> Status;

    auto remove_timer(Timer &timer) -> void;

    gktl::Canary m_canary;
    CpuNum m_cpunum;
    Timer *m_running;
    ClockSource *m_clock_source;
    TimePoint<> m_next_timer_deadline = TimePoint<>::max();

    // Using this field to unify benchmark among different clocks. Calculated by
    // Offset = ClockTime - MonoticTime.
    Duration m_offset;
    ustl::collections::intrusive::Set<Timer> m_active_timers;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER-QUEUE_HPP