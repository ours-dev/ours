#include <ours/time/timer_queue.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/clock.hpp>

#include <ours/task/scheduler.hpp>
#include <ours/irq/mod.hpp>

#include <arch/interrupt.hpp>
#include <ustl/chrono/duration.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <algorithm>

namespace ours::time {
    auto TimerQueue::update_next_deadline() -> void {
        auto deadline = m_next_timer_deadline;
        if (!m_active_timers.empty()) {
            deadline = min(m_active_timers.begin()->deadline().latest(), deadline);
        }

        TickDevice::Current::get().program_event(deadline);
        m_next_timer_deadline = deadline;
    }

    auto TimerQueue::insert_timer(Timer &timer, TimePoint<> earliest, TimePoint<> latest) -> Status {
        (void)m_active_timers.insert(timer);
        update_next_deadline();

        return Status::Ok;
    }

    auto TimerQueue::remove_timer(Timer &timer) -> void {
        DEBUG_ASSERT(timer.timer_queue() == this);
    }

    auto TimerQueue::run_expired_timers(CpuNum cpu) -> usize {
        m_canary.verify<Self>();

        auto const now = SteadyClock::now();
        log::trace("CPU[{}]-time={} ns", cpu, now.time_since_epoch().count());

        m_next_timer_deadline = kInfiniteTime;

        usize num_expired = 0;
        while (!m_active_timers.empty()) {
            auto &timer = *m_active_timers.begin();
            if (timer.deadline().latest() > now) {
                break;
            }
            m_running = &timer;
            num_expired += 1;
            (void)m_active_timers.erase(m_active_timers.iterator_to(timer));

            auto const result = timer.on_expired();
            if (result == Timer::HandlerResult::Restart) {
                // FIXME(SmallHuaZi) Finish Timer::forward()
                // If the timer is rescheduled, we need to reinsert it.
                timer.forward(Duration::min());
            }

            m_running = nullptr;
        }

        update_next_deadline();
        return num_expired;
    }

} // namespace ours::time