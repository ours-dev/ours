#include <ours/time/timer_scheduler.hpp>

namespace ours::time {
    auto TimerScheduler::run_queues() -> void {
        m_canary.verify<Self>();

        for (auto &queue : perclock_) {
            queue.run_expired_timers(cpunum_);
        }
    }

    auto TimerScheduler::activate_timer(Timer &timer) -> void {

    }

} // namespace ours::time