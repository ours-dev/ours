#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <prof/sampler.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_scheduler.hpp>

namespace ours::time {
TickSched::TickSched()
    : m_canary()
    , m_timer(ClockId::Monotonic)
    , m_workmode(WorkMode::Shutdown)

{}

auto TickSched::emulate(WorkMode workmode) -> void {
    if (m_workmode == workmode) {
        return;
    }

    m_timer.set_deadline({});
    if (WorkMode::HpTick == workmode) {
        m_timer.activate({}, handle_event);
    } else {
    }

    m_workmode = workmode;
}

auto TickSched::handle_event(Timer *timer) -> Timer::HandlerResult {
    auto const now = SteadyClock::now();
    task::Scheduler::Current::tick(now);

    // The sampling profiler's timer-fallback source samples the interrupted
    // context here; a no-op unless 'prof.start tick' is running.
    prof::Sampler::get().on_scheduler_tick();

    // First we execute those expired local timers.
    TimerScheduler::Current::run_local_queues();

    timer->forward(NSEC_PER_TICK);
    return Timer::HandlerResult::Restart;
}

} // namespace ours::time