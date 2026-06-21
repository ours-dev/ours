/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_SCHEDULER_HPP
#define OURS_TIME_TIMER_SCHEDULER_HPP 1

#include <ours/time/timer-queue.hpp>

namespace ours::time {
    /// Per-CPU timer scheduler that manages the execution of timers.
    class TimerScheduler {
        using Self = TimerScheduler;
      public:
        class Current;

        auto run_queues() -> void;
      private:
        friend class Timer;

        static auto activate_timer(Timer &timer) -> void;

        gktl::Canary m_canary;
        CpuNum cpunum_;
        TimerQueue perclock_[usize(ClockId::MaxNumClocks)];
    };

    class TimerScheduler::Current {
        using Singleton = CpuLocal::Current<TimerScheduler>;
      public:
        FORCE_INLINE
        static auto run_local_queues() -> void {
            Singleton::get().run_queues();
        }
    };

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER_SCHEDULER_HPP