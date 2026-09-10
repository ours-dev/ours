/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_SCHED_HPP
#define OURS_TIME_TICK_SCHED_HPP 1

#include <ours/mm/cpu_local.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/timer.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::time {
/// What a tick *means* on this CPU, separated from where it comes from.
///
/// A tick is the scheduler's heartbeat: account the running thread's time
/// and maybe preempt it, take a profiler sample, run the local timers that
/// have expired. That is on_tick(), and it is Linux's update_process_times()
/// plus run_local_timers(). Who calls it depends on the work mode:
///
///  * Legacy: the tick device fires every 1/HZ and its periodic handler
///    (TickDevice::handle_periodic_event) calls on_tick() directly. Nothing
///    here is armed; the hardware is the metronome.
///
///  * HpTick: the device runs in oneshot mode and is armed only for the
///    next deadline the timer queue knows about, so the tick has to be a
///    Timer like any other, re-armed one period ahead each time it fires.
///    This is how Linux's tick_sched_timer works once highres is on, and it
///    is the prerequisite for stopping the tick while idle.
///
/// Shutdown is the state before the scheduler exists. Ticks do arrive then
/// -- the PIT is enrolled at Irq + 1 and the local APIC calibrates itself by
/// counting them -- and on_tick() ignores them, because there is no thread
/// to account and no Scheduler to hand them to until task::init() has run.
class TickSched: public mm::CpuLocal::Current<TickSched> {
    using Self = TickSched;

  public:
    enum class WorkMode {
        Shutdown,

        // Legacy periodic tick mode, frequency dependent on the OURS_CONFIG_HZ
        Legacy,

        // High precision tick mode, based on high precision clock device.
        HpTick,
    };

    TickSched();

    ~TickSched() = default;

    /// Switches how this CPU's tick is produced. Legacy and Shutdown may be
    /// set from any CPU (they only flip a mode); HpTick must be entered on
    /// the CPU itself, because the tick Timer it arms goes on the activating
    /// CPU's queue.
    auto emulate(WorkMode workmode) -> void;

    /// One tick on this CPU, at |now|. Called from interrupt context, either
    /// by the tick device's periodic handler or by the tick Timer.
    auto on_tick(TimePoint<> now) -> void;

    FORCE_INLINE
    auto is_legacy() const -> bool {
        return m_workmode == WorkMode::Legacy;
    }

    FORCE_INLINE
    auto is_hptick() const -> bool {
        return m_workmode == WorkMode::HpTick;
    }

  private:
    friend class TickDevice;
    static auto handle_event(Timer *) -> Timer::HandlerResult;

    gktl::Canary m_canary;
    Timer m_timer;
    WorkMode m_workmode;
    Ticks m_last_tick;
    Ticks m_next_tick;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_SCHED_HPP
