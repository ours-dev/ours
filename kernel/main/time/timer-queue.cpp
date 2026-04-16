#include <ours/time/timer-queue.hpp>
#include <ours/time/tick-device.hpp>
#include <ours/time/clock.hpp>

#include <ours/task/scheduler.hpp>
#include <ours/irq/mod.hpp>

#include <arch/interrupt.hpp>
#include <ustl/chrono/duration.hpp>
#include <gktl/init_hook.hpp>

namespace ours::time {
    auto TimerQueue::update_next_deadline() -> void {
        using ustl::algorithms::min;

        auto deadline = next_timer_deadline_;
        if (!actives_.empty()) {
            deadline = min(actives_.begin()->deadline().latest(), deadline);
        }

        TickDevice::Current::get().program_event(deadline);
        next_timer_deadline_ = deadline;
    }

    auto TimerQueue::insert_timer(Timer &timer, TimePoint<> earliest, TimePoint<> latest) -> Status {
        actives_.insert(timer);
        update_next_deadline();

        return Status::Ok;
    }

    auto TimerQueue::remove_timer(Timer &timer) -> void {
        DEBUG_ASSERT(timer.timer_queue() == this);
    }

    auto TimerQueue::run_expired_timers(CpuNum cpu) -> usize {
        canary_.verify();

        auto const now = SteadyClock::now();
        log::trace("CPU[{}]-time={} ns", cpu, now.time_since_epoch().count());

        next_timer_deadline_ = kInfiniteTime;

        usize num_expired = 0;
        while (!actives_.empty()) {
            auto &timer = *actives_.begin();
            if (timer.deadline().latest() > now) {
                break;
            }
            running_ = &timer;
            num_expired += 1;
            actives_.erase(actives_.iterator_to(timer));

            auto const result = timer.on_expired();
            if (result == Timer::HandlerResult::Restart) {
                // FIXME(SmallHuaZi) Finish Timer::forward()
                // If the timer is rescheduled, we need to reinsert it.
                timer.forward(Duration::min());
            }

            running_ = nullptr;
        }

        update_next_deadline();
        return num_expired;
    }

} // namespace ours::time