#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_queue.hpp>

#include <gktl/init_hook.hpp>
#include <prof/sampler.hpp>

namespace ours::time {
TickSched::TickSched()
    : m_canary(this)
    , m_timer(ClockId::Monotonic)
    , m_workmode(WorkMode::Shutdown)
    , m_last_tick(0)
    , m_next_tick(0) {}

auto TickSched::emulate(WorkMode workmode) -> void {
    m_canary.verify<Self>();

    if (m_workmode == workmode) {
        return;
    }

    // Leaving HpTick: the tick Timer must stop, or it would keep calling
    // on_tick() alongside whatever replaces it.
    if (m_workmode == WorkMode::HpTick) {
        m_timer.cancel();
    }

    if (workmode == WorkMode::HpTick) {
        auto const now = SteadyClock::now();
        m_timer.activate(Deadline::at(now + NSEC_PER_TICK), handle_event);
    }

    m_workmode = workmode;
}

/// Linux's update_process_times(): the three consumers of a tick, in the
/// order Linux runs them.
auto TickSched::on_tick(TimePoint<> now) -> void {
    m_canary.verify<Self>();

    // Before task::init() there is no scheduler state to touch; the ticks
    // that arrive then exist only to advance jiffies, and the tick device
    // has already done that before calling here.
    if (m_workmode == WorkMode::Shutdown) {
        return;
    }

    task::Scheduler::Current::tick(now);

    // The sampling profiler's timer-fallback source samples the interrupted
    // context here; a no-op unless 'prof.start tick' is running.
    prof::Sampler::get().on_scheduler_tick();

    TimerQueue::get().run_expired_timers();
}

auto TickSched::handle_event(Timer *timer) -> Timer::HandlerResult {
    TickSched::get().on_tick(SteadyClock::now());

    timer->forward(NSEC_PER_TICK);
    return Timer::HandlerResult::Restart;
}

namespace {
    /// Every CPU's tick goes live once every CPU's scheduler exists.
    ///
    /// task::init() builds the Scheduler of every possible CPU from the BSP
    /// and main.cpp publishes InitLevel::Threading right after it, so a hook
    /// at that level is the first moment on_tick() has something to call.
    /// Doing this per CPU from the BSP, rather than when each AP comes up,
    /// mirrors task::init() itself: an AP's first tick after it is online
    /// should already reach its scheduler.
    INIT_CODE
    auto start_legacy_tick() -> void {
        cpu_possible_mask().for_each(
            [](CpuNum cpu) { TickSched::get(cpu).emulate(TickSched::WorkMode::Legacy); });
    }
    GKTL_INIT_HOOK(StartLegacyTick, start_legacy_tick, gktl::InitLevel::Threading);
} // namespace

} // namespace ours::time
