#include <ours/time/timer-scheduler.hpp>

namespace ours::time {
    auto TimerScheduler::run_queues() -> void {
        canary_.verify();

        for (auto &queue : perclock_) {
            queue.run_expired_timers(cpunum_);
        }
    }

    auto TimerScheduler::activate_timer(Timer &timer) -> void {

    }

} // namespace ours::time