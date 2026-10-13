#include <ours/task/sched/sched_clock.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer_queue.hpp>

namespace ours::task::sched {
using time::SteadyClock;
using time::TimerQueue;

auto SchedClock::update_clock() -> SchedTime {
    if (!!(m_clock_flags & ClockFlags::ActSkip)) {
        return m_sched_clock;
    }

    // The kernel clock is typed; the scheduler's arithmetic is not. This is
    // one of the three places the boundary is crossed (see to_sched_time in
    // sched/types.hpp), and crossing it here rather than deeper keeps every
    // expression below in a single arithmetic.
    auto const now = to_sched_time(SteadyClock::now());
    auto const delta = now - m_prev_real_clock;

    // |now|, not m_sched_clock. This field is the previous *real* reading, and
    // it is the only thing that makes |delta| above mean "real time elapsed
    // since the last update". Storing the scheduler clock into it crossed the
    // two: the next delta was then measured from the virtual clock, which lags
    // the real one, and the result was a six-update cycle in which the
    // scheduler clock advanced 10, 20, 20, 10, 0, 0 for six equal real steps
    // of 10. The long-run rate came out exactly right -- which is why nothing
    // ever showed -- but two updates in six charged the running thread
    // nothing at all and two charged it double.
    //
    // A skipped update (ActSkip, above) deliberately does not reach here, so
    // the interval it covers is folded into the next delta rather than lost.
    // That is the difference between deferring the clock and freezing it, and
    // the request/accept handshake wants the former.
    m_prev_real_clock = now;
    if (delta > SchedDuration{0}) {
        m_sched_clock += delta;
    }

    m_clock_flags = ClockFlags::Updated;
    return m_sched_clock;
}

/// Into this CPU's timer queue, not the device: the queue is the device's one
/// writer and arms it for the earlier of this and its own head, so a timer due
/// before the slice ends still fires on time. Writing the device from here, as
/// this once did, left the queue's record of what it armed wrong.
auto SchedClock::reset_preempt(SchedDuration delta) -> void {
    // Back across the boundary: the queue takes a TimePoint, so the deadline
    // is assembled in the scheduler's arithmetic and converted once.
    auto const deadline = to_sched_time(SteadyClock::now()) + delta;
    TimerQueue::get().reset_preempt(to_time_point(deadline));
}

auto SchedClock::cancel_preempt() -> void {
    TimerQueue::get().reset_preempt(INFINITE_TIME);
}

} // namespace ours::task::sched