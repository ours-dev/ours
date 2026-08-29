#include <ours/time/timer.hpp>
#include <ours/time/timer_scheduler.hpp>

namespace ours::time {
    auto Timer::activate(Deadline deadline, Handler on_expired) -> void {
        m_canary.verify<Self>();

        m_deadline = deadline;
        m_on_expired = on_expired;
        m_cancalled.store(false, std::memory_order_release);

        TimerScheduler::activate_timer(*this);
    }

    auto Timer::cancel() -> void {
        // Just mark it. Best to not manully remove it from timer queue,
        // there is lock contests.
        m_cancalled.store(true, std::memory_order_relaxed);
    }

    auto Timer::forward(Duration interval) -> void {

    }

} // namespace ours::time