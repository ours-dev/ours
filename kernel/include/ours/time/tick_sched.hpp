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
/// A tick no longer drives the scheduler: a slice ends at the CPU's
/// preemption deadline, a slot in its timer queue (TimerQueue::reset_preempt()).
/// What is left is the timer queue itself, when the tick is what drives it:
/// on_tick() runs the local timers that have expired, the preemption deadline
/// among them. Who calls it depends on how this CPU's tick device is run, and
/// TickDevice is the one place that says so:
///
///  * Periodic: the device fires every 1/HZ and its periodic handler
///    (TickDevice::handle_periodic_event) calls on_tick() directly. Nothing
///    here is armed; the hardware is the metronome, on_tick() is the only
///    thing that looks at the timer queue, and a slice ends at the first
///    tick past its deadline.
///
///  * Oneshot: the device is armed only for the next deadline the timer
///    queue knows about, the preemption deadline included, so the tick has to
///    be a Timer like any other, re-armed one period ahead each time it fires
///    (arm_timer(), called once when the device enters Oneshot). The device
///    interrupt runs the queue, the tick among it. In this mode the tick only
///    advances jiffies, and is the next thing to go.
///
/// The other fact a tick depends on is whether the scheduler exists, kept
/// here and nowhere else (start()). Ticks do arrive before it does -- the PIT
/// is enrolled at Irq + 1 and the local APIC calibrates itself by counting
/// them, and a one-shot CPU's tick timer starts the moment its device is up --
/// and until start() on_tick() does nothing with them, because there is no
/// thread to preempt and no Scheduler to wake one until task::init() has run.
class TickSched: public mm::CpuLocal::Current<TickSched> {
    using Self = TickSched;

  public:
    TickSched();

    ~TickSched() = default;

    /// This CPU's scheduler exists: from now on a tick is handed to it. Only
    /// publishes that fact, so it may be called for any CPU from any CPU, as
    /// the start hook does for all of them from the boot CPU.
    auto start() -> void;

    /// One tick on this CPU, at the time given. Called from interrupt
    /// context, either by the tick device's periodic handler or by the tick
    /// Timer.
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

    /// Disarms the tick Timer, on this CPU as it goes offline, so that it is
    /// not among the timers another CPU takes over: there it would tick that
    /// CPU a second time. arm_timer() arms it again when the CPU is back.
    auto cancel_timer() -> void;

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
