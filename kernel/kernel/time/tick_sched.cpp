#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_queue.hpp>

#include <gktl/init_hook.hpp>

namespace ours::time {
TickSched::TickSched()
    : m_canary(this)
    , m_timer(ClockId::Monotonic)
    , m_started(false)
    , m_last_tick(0)
    , m_next_tick(0) {}

auto TickSched::start() -> void {
    m_canary.verify<Self>();
    m_started.store(true, std::memory_order_release);
}

auto TickSched::arm_timer() -> void {
    m_canary.verify<Self>();
    m_timer.activate(Deadline::at(SteadyClock::now() + NSEC_PER_TICK), handle_event);
}

/// The handler runs only on this CPU, and this CPU is not in it, so the
/// cancel never waits.
auto TickSched::cancel_timer() -> void {
    m_canary.verify<Self>();
    auto _ = m_timer.cancel();
}

/// The one consumer of a tick, when this CPU's device is Periodic: this CPU's
/// expired timers, the preemption deadline among them. The scheduler is not a
/// consumer any more: a slice ends at the preemption deadline, which a
/// one-shot device is armed for and a periodic tick finds passed
/// (TimerQueue::run_expired_timers()). Neither are the sampling profiler and
/// the lockup detector; each arms timers of its own (kernel/lib/sampler,
/// kernel/lib/lockup_detector).
auto TickSched::on_tick(TimePoint<>) -> void {
    m_canary.verify<Self>();

    // Before task::init() there is no thread a preemption deadline could
    // name and no timer a handler could wake one for; the ticks that arrive
    // then exist only to advance jiffies.
    if (!is_started()) {
        return;
    }

    // With a Periodic device the tick is the only thing that looks at the
    // queue, because the device is not armed for the queue's deadlines (see
    // TickDevice::program_event()). With a Oneshot one the tick *is* a timer,
    // called from run_expired_timers() itself, which the device interrupt runs
    // on its own: running it again from here would re-enter the queue from
    // inside its own loop, resetting its armed deadline and overwriting the
    // record of which handler is running.
    if (TickDevice::Current::is_mode_periodic()) {
        TimerQueue::get().run_expired_timers();
    }
}

/// The tick as a timer: the consumers of a tick, then the next period.
///
/// Jiffies advance here too, on the one CPU that owns them, because the
/// periodic device's handler that did it is not this CPU's handler any more.
/// forward() says how many periods went by, one when the handler ran on time,
/// so jiffies stays a count of periods even if this CPU was kept from the
/// handler for longer than one.
auto TickSched::handle_event(Timer *timer) -> Timer::HandlerResult {
    TickSched::get().on_tick(SteadyClock::now());

    auto const periods = timer->forward(NSEC_PER_TICK);
    if (Cpu::id() == ClockSource::Default::CPUNUM) {
        ClockSource::Default::advance_ticks(static_cast<Ticks>(periods));
    }
    return Timer::HandlerResult::Restart;
}

namespace {
    /// Every CPU's scheduler is told to expect ticks once every CPU's
    /// scheduler exists.
    ///
    /// task::init() builds the Scheduler of every possible CPU from the BSP
    /// and main.cpp publishes InitLevel::Threading right after it, so a hook
    /// at that level is the first moment on_tick() has something to call.
    /// Doing this per CPU from the BSP, rather than when each AP comes up,
    /// mirrors task::init() itself: an AP's first tick after it is online
    /// should already reach its scheduler.
    ///
    /// How a CPU's tick is produced is not decided here. A Periodic device
    /// calls on_tick() from its handler; a CPU whose device entered Oneshot
    /// armed its tick timer then (TickDevice::setup_oneshot_tick()), on the
    /// boot CPU long before this level.
    INIT_CODE
    auto start_ticks() -> void {
        cpu_possible_mask().for_each([](CpuNum cpu) { TickSched::get(cpu).start(); });
    }
    GKTL_INIT_HOOK(StartTicks, start_ticks, gktl::InitLevel::Threading);
} // namespace

} // namespace ours::time
