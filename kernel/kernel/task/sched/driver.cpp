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

    // The kernel clock is typed; the scheduler's arithmetic is not. This is
    // one of the three places the boundary is crossed (see to_sched_time in
    // sched/types.hpp), and crossing it here rather than deeper keeps every
    // expression below in a single arithmetic.
    auto const now = to_sched_time(SteadyClock::now());
    auto const delta = now - m_prev_real_clock;

    m_prev_real_clock = m_sched_clock;
    if (delta > SchedDuration{0}) {
        m_sched_clock += delta;
    }

    m_clock_flags = ClockFlags::Updated;
    return m_sched_clock;
}

auto Driver::reset_preempt(SchedDuration delta) -> void {
    // Back across the boundary: the tick device takes a TimePoint, so the
    // deadline is assembled in the scheduler's arithmetic and converted once.
    auto const deadline = to_sched_time(SteadyClock::now()) + delta;
    TickDevice::Current::get().program_event(to_time_point(deadline));
}

} // namespace ours::task::sched