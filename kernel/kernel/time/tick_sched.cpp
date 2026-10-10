#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_queue.hpp>

#include <gktl/init_hook.hpp>
#include <lockup_detector/lockup_detector.hpp>

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

/// The consumers of a tick, in order: the lockup detector's heartbeat, the
/// scheduler's tick, then -- in Legacy only -- this CPU's expired timers. The
/// sampling profiler is not one of them; it arms timers of its own while it
/// runs (kernel/lib/sampler), so a tick costs nothing extra when nobody is
/// profiling.
auto TickSched::on_tick(TimePoint<> now) -> void {
    m_canary.verify<Self>();

    // The heartbeat the other CPUs check, and once a second this CPU's check
    // of theirs. First, and in every mode: a tick that arrives is the
    // evidence this CPU still takes interrupts, whatever is done with it.
    lockup_detector::on_tick();

    // Before task::init() there is no scheduler state to touch; the ticks
    // that arrive then exist only to advance jiffies, and the tick device
    // has already done that before calling here.
    if (m_workmode == WorkMode::Shutdown) {
        return;
    }

    // The scheduler works in bare nanoseconds (sched::SchedTime); the clock
    // typing stops at its door.
    task::Scheduler::Current::tick(task::sched::to_sched_time(now));

    // In Legacy the tick is the only thing that looks at the queue, because
    // the device is not armed for the queue's deadlines (see
    // TickDevice::program_event()). In HpTick the tick *is* a timer, called
    // from run_expired_timers() itself, which the device interrupt runs on
    // its own: running it again from here would re-enter the queue from
    // inside its own loop, resetting its armed deadline and overwriting the
    // record of which handler is running.
    if (m_workmode == WorkMode::Legacy) {
        TimerQueue::get().run_expired_timers();
    }
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
