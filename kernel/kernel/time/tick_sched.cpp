#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_queue.hpp>

#include <gktl/init_hook.hpp>

namespace ours::time {
TickSched::TickSched() : m_canary(this), m_started(false) {}

auto TickSched::start() -> void {
    m_canary.verify<Self>();
    m_started.store(true, std::memory_order_release);
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
    // TickDevice::program_event()). A CPU in Oneshot has no tick: only a
    // straggling interrupt of the periodic device it replaced can reach here,
    // and its queue is its own device's interrupt to run.
    if (TickDevice::Current::is_mode_periodic()) {
        TimerQueue::get().run_expired_timers();
    }
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
    /// Only a Periodic device calls on_tick(), from its handler; a CPU whose
    /// device entered Oneshot has no tick at all (TickDevice::setup_oneshot_tick()).
    INIT_CODE
    auto start_ticks() -> void {
        cpu_possible_mask().for_each([](CpuNum cpu) { TickSched::get(cpu).start(); });
    }
    GKTL_INIT_HOOK(StartTicks, start_ticks, gktl::InitLevel::Threading);
} // namespace

} // namespace ours::time
