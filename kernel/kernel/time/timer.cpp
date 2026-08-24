#include <ours/time/timer.hpp>
#include <ours/time/timer-scheduler.hpp>

using ustl::sync::MemoryOrder;

namespace ours::time {
    auto Timer::activate(Deadline deadline, Handler on_expired) -> void {
        canary_.verify();

        m_deadline = deadline;
        m_on_expired = on_expired;
        m_cancalled.store(false, MemoryOrder::Release);

        TimerScheduler::activate_timer(*this);
    }

    auto Timer::cancel() -> void {
        // Just mark it. Best to not manully remove it from timer queue,
        // there is lock contests.
        m_cancalled.store(true, MemoryOrder::Relaxed);
    }

    auto Timer::forward(Duration interval) -> void {

    }

} // namespace ours::task