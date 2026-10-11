/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_SCHED_HPP
#define OURS_TIME_TICK_SCHED_HPP 1

#include <ours/mm/cpu_local.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/timer.hpp>

#include <atomic>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::time {
/// What a tick *means* on this CPU, separated from where it comes from.
///
/// A tick is the scheduler's heartbeat: account the running thread's time
/// and maybe preempt it, and, when the tick is what drives the timer queue,
/// run the local timers that have expired. That is on_tick(), which does them
/// in that order. Who calls it depends on how this CPU's tick device is run,
/// and TickDevice is the one place that says so:
///
///  * Periodic: the device fires every 1/HZ and its periodic handler
///    (TickDevice::handle_periodic_event) calls on_tick() directly. Nothing
///    here is armed; the hardware is the metronome, and on_tick() is the
///    only thing that looks at the timer queue.
///
///  * Oneshot: the device is armed only for the next deadline the timer
///    queue knows about, so the tick has to be a Timer like any other,
///    re-armed one period ahead each time it fires (arm_timer(), called once
///    when the device enters Oneshot). The device interrupt runs the queue,
///    the tick among it. A tick that is a Timer is also the prerequisite for
///    stopping the tick while idle.
///
/// The other fact a tick depends on is whether the scheduler exists, kept
/// here and nowhere else (start()). Ticks do arrive before it does -- the PIT
/// is enrolled at Irq + 1 and the local APIC calibrates itself by counting
/// them, and a one-shot CPU's tick timer starts the moment its device is up --
/// and until start() on_tick() gives them to the lockup detector's heartbeat
/// and to nothing else, because there is no thread to account and no
/// Scheduler to hand them to until task::init() has run.
class TickSched: public mm::CpuLocal::Current<TickSched> {
    using Self = TickSched;

  public:
    TickSched();

    ~TickSched() = default;

    /// This CPU's scheduler exists: from now on a tick is handed to it. Only
    /// publishes that fact, so it may be called for any CPU from any CPU, as
    /// the start hook does for all of them from the boot CPU.
    auto start() -> void;

    /// One tick on this CPU, at |now|. Called from interrupt context, either
    /// by the tick device's periodic handler or by the tick Timer.
    auto on_tick(TimePoint<> now) -> void;

    /// Whether the scheduler has been told to expect ticks.
    [[nodiscard]]
    FORCE_INLINE
    auto is_started() const -> bool {
        return m_started.load(std::memory_order_acquire);
    }

  private:
    friend class TickDevice;

    /// Arms the tick as a Timer on the current CPU's queue, one period from
    /// now. Called once per CPU, by TickDevice, on that CPU, when its device
    /// enters Oneshot; a second call fails the Timer's own assertion that it
    /// is not queued.
    auto arm_timer() -> void;

    static auto handle_event(Timer *) -> Timer::HandlerResult;

    gktl::Canary m_canary;
    Timer m_timer;
    /// Set once, by start(), and read by this CPU's ticks. The release store
    /// publishes the scheduler task::init() built; the acquire load in a tick
    /// is what makes it safe to hand the tick to it.
    std::atomic<bool> m_started;
    Ticks m_last_tick;
    Ticks m_next_tick;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_SCHED_HPP
