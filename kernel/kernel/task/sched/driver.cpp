#include <ours/task/sched/driver.hpp>

#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>

namespace ours::task::sched {
    using time::SteadyClock;
    using time::TickDevice;

    auto Driver::update_clock() -> SchedTime {
        if (!!(m_clock_flags & ClockFlags::ActSkip)) {
            return m_sched_clock;
        }

        auto const now = SteadyClock::now();
        auto const delta = now - m_prev_real_clock;

        m_prev_real_clock = m_sched_clock;
        if (delta > decltype(delta)::zero()) {
            m_sched_clock += delta;
        }

        m_clock_flags = ClockFlags::Updated;
        return m_sched_clock;
    }

    auto Driver::reset_preempt(SchedDuration delta) -> void {
        auto const now = SteadyClock::now();
        TickDevice::Current::get().program_event(now + delta);
    }

} // namespace ours::task::sched