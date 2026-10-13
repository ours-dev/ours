/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_DEVICE_HPP
#define OURS_TIME_TICK_DEVICE_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock_event_device.hpp>

#include <arch/auto_interrupt_disabler.hpp>

namespace ours::time {
class TickBroadcast;

/// Each CPU's view of "the device that interrupts me": a TickDevice is the
/// per-CPU binding between the tick layer above (TickSched, TimerQueue) and
/// whichever ClockEventDevice is currently best for this CPU.
///
/// It owns three decisions, and nothing else does:
///
///  * Which device this CPU uses. Drivers do not pick; they enroll, and
///    check_new_device() decides per CPU -- a per-CPU device beats a global
///    one, oneshot-capable beats periodic-only, then rating. The rules are
///    written out on check_percpu() and check_preferred().
///
///  * How the device is driven. In Periodic mode the device fires every
///    1/HZ and handle_periodic_event() is its handler; a device that cannot
///    do periodic natively is re-armed one tick ahead from that handler. In
///    Oneshot mode the device is armed for exactly the next deadline the
///    timer queue asks for through program_event(), and
///    handle_oneshot_event() is its handler. Switching devices keeps the
///    mode, the handler and the pending deadline (rebind_device()).
///
///    A CPU starts Periodic and enters Oneshot once, when its own timer
///    device is enrolled with the boot option kernel.time.oneshot on
///    (setup_oneshot_tick()); it never leaves. Until then -- the boot CPU's
///    first ticks come from the global HPET or PIT, which the local APIC
///    calibrates itself against -- the tick is the device's own metronome.
///    From then on the timer queue is the only thing that arms the device,
///    and the tick is a Timer in that queue (TickSched).
///
///  * When the device is silent. In Oneshot mode a program_event(INFINITE)
///    parks the device in OneshotStopped, and the next deadline wakes it.
///
/// The binding is per CPU because a global device (PIT, HPET) can only
/// interrupt one CPU at a time, so every other CPU needs its own local APIC
/// timer. A global device can still serve several CPUs through a
/// TickBroadcast group, by IPI; that is not what selection does -- a device
/// that loses the selection is simply left aside -- but what replace_with()
/// installs, for a CPU whose own device has to be given up.
class TickDevice {
    using Self = TickDevice;

  public:
    class Current;

    enum class Mode {
        Periodic,
        Oneshot,
    };

    /// How many times a device is given to take a deadline before it is
    /// retired, counting the one that was refused. A refusal that reaches the
    /// timer queue has already been retried at growing deltas by the device
    /// layer (ClockEventDevice::program_min_delta()), so this is for a
    /// refusal that clears by itself in the time it takes to ask again: a
    /// stall of the host under emulation, or an SMI landing in the window. It
    /// is asked back to back, as there is nothing to wait with: a CPU whose
    /// device has just refused has no timer to wait on.
    static constexpr u32 ATTEMPTS_BEFORE_RETIRING = 3;

    TickDevice();

    ~TickDevice() = default;

    /// Programs next tick event with |expired|.
    ///
    /// Return Status::Ok on success, or an error code on failure.
    auto program_event(TimePoint<> expired) -> Status;

    /// Moves this CPU's Oneshot tick from the device it is on to |dev|: the
    /// CPU's own device when |group| is null, or a global device that serves
    /// it through |group| (TickBroadcast) when not. The timer queue is armed
    /// again on the new device before this returns. On failure |dev| is marked
    /// failed, and the CPU is put back on the device it left, if that can be
    /// done and that device has not failed itself.
    ///
    /// The one way a CPU changes devices after it has entered Oneshot: it
    /// does not go back to a Periodic one. Must run on this CPU with
    /// interrupts off, like check_new_device(), and |group| must have been
    /// fetched beforehand (TickBroadcast::for_device() takes a lock this must not).
    auto replace_with(ClockEventDevice &dev, TickBroadcast *group) -> Status;

    /// This CPU's timer queue says the device in service refused a deadline,
    /// after the device layer had retried. The device is asked again, twice
    /// more and straight away; if it takes the deadline that is the end of it.
    /// A device that refuses all three is marked failed and this CPU's tick
    /// is moved, there and then, to the best device that has not failed,
    /// trying the next best when one will not take it; see
    /// pick_replacement(). If none is left the CPU has no tick, and says so.
    ///
    /// Done in the context that found out and not handed to a thread. A CPU
    /// whose device has just failed has no timer interrupt to preempt with,
    /// so a worker thread asked to do this would wait behind whatever thread
    /// the CPU happens to be running, for ever if that one never blocks, and
    /// the lockup detector would find the CPU first. Hence nothing here may
    /// take a mutex: the devices to choose among are read from a table that
    /// needs no lock (ClockEventDevice::for_each_enrolled()).
    ///
    /// On this CPU with interrupts off, as replace_with() requires; the timer
    /// queue calls it so (TimerQueue::notify_refusal()).
    auto on_device_refused() -> void;

    /// This CPU is going offline: its tick stops, and nothing interrupts it
    /// for a timer again until restart_after_offline(). A Oneshot tick's
    /// Timer is cancelled, so it does not go with the queue's other timers
    /// to the CPU that takes them over; the device is shut down with its
    /// handler and binding kept, or, served through a broadcast group, the
    /// CPU leaves the group. On this CPU with interrupts off.
    auto stop_for_offline() -> void;

    /// The reverse, on the CPU coming back, interrupts off: the device it had
    /// is put back into service in the mode it had, or the CPU rejoins its
    /// group, and a Oneshot tick is a Timer in its queue again.
    auto restart_after_offline() -> Status;

    /// The broadcast group this CPU's tick is served through, or null when
    /// the CPU has a device of its own.
    [[nodiscard]]
    FORCE_INLINE
    auto group() const -> TickBroadcast * {
        return m_group;
    }

    /// Offers |new_clkdev| to this CPU. It is installed -- replacing the
    /// current device, if any -- when it can interrupt this CPU and is
    /// preferable to what is bound now; see check_percpu() and
    /// check_preferred() for what "preferable" means.
    ///
    /// Must run on the CPU it is deciding for: a driver enrolls on the CPU
    /// whose tick it is offering to drive. And with interrupts off: the
    /// device being replaced may still be interrupting this CPU, and its
    /// handler is moved out from under that interrupt. ClockEventDevice::enroll()
    /// sees to it.
    ///
    /// Returns Status::Ok when |new_clkdev| was installed, Status::AlreadyBound
    /// when this CPU keeps the device it has, or the error that stopped the
    /// installation part-way.
    auto check_new_device(ClockEventDevice *new_clkdev) -> Status;

    /// Installs |new_clkdev| unconditionally: detaches the current device
    /// and moves the mode, handler and pending deadline over. What
    /// check_new_device() does once it has decided; exposed for callers
    /// that have made the decision themselves. Interrupts must be off, as for
    /// check_new_device().
    auto rebind_device(ClockEventDevice *new_clkdev) -> Status;

    /// False while this CPU still points at the placeholder device.
    [[nodiscard]]
    auto has_device() const -> bool;

    [[nodiscard]]
    FORCE_INLINE
    auto device() const -> ClockEventDevice * {
        return m_clkdev;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto mode() const -> Mode {
        return m_mode;
    }

  protected:
    /// Update the jiffies count by delta |num_ticks|. This function is called
    /// by the clock event device when it handles a tick event.
    ///
    /// Requires:
    ///   1). The current CPU is ClockSource::Default::CPUNUM.
    static auto update_jiffies_tick(Ticks num_ticks) -> void;

    static auto update_jiffies_time(Time now) -> void;

    /// A common clock event handler working on HZ mode for periodic events.
    static auto handle_periodic_event(ClockEventDevice *) -> void;

    /// The handler of a Oneshot device: the device fired for the earliest
    /// deadline of this CPU's timer queue, so run what is due. The queue arms
    /// the device again for what is left.
    static auto handle_oneshot_event() -> void;

  private:
    /// A device is per-CPU when its mask names exactly |cpu|. The local
    /// APIC timer sets such a mask on itself; PIT and HPET carry the whole
    /// possible mask.
    static auto is_percpu(ClockEventDevice const &dev, CpuNum cpu) -> bool;

    /// Whether |newdev| may serve |cpu| at all.
    [[nodiscard]]
    auto check_percpu(ClockEventDevice const &newdev, CpuNum cpu) const -> bool;

    /// Whether |newdev| is better than the current device, given that both
    /// may serve this CPU.
    [[nodiscard]]
    auto check_preferred(ClockEventDevice const &newdev) const -> bool;

    /// Puts |dev| into service for the periodic tick. A CPU that runs Oneshot
    /// changes devices through replace_with() instead.
    auto setup_periodic(ClockEventDevice *dev) -> Status;

    /// The device that would serve this CPU's tick in place of the one that
    /// failed, and the group it would serve through when it is global; both
    /// null when there is none. No lock is taken. The best device that has not failed, can be
    /// armed for one instant and can serve this CPU: its own beats a global
    /// one, then the higher rating wins, as in selection -- except that the
    /// only global device that counts is the one whose interrupt the legacy
    /// timer line delivers, since a global device whose line goes elsewhere
    /// would be put in service and never heard.
    struct Replacement {
        ClockEventDevice *device = nullptr;
        TickBroadcast *group = nullptr;
    };
    auto pick_replacement(CpuNum cpu) -> Replacement;

    /// Whether |dev|, which is about to serve this CPU, should be run
    /// one-shot from the start: the boot option is on and |dev| is this CPU's
    /// own and can be armed for one instant. The global HPET and PIT never
    /// are: they are the early-boot metronome and stay Periodic.
    [[nodiscard]]
    auto wants_oneshot_tick(ClockEventDevice const &dev, CpuNum cpu) const -> bool;

    /// Puts |dev| into service in Oneshot mode with the timer queue as its
    /// only writer, and makes the tick a timer in that queue. Not an
    /// ordinary Periodic-to-Oneshot switch: nothing of the old device's
    /// handler or deadline is carried over. On failure the mode is
    /// unchanged and the caller falls back to setup_periodic().
    auto setup_oneshot_tick(ClockEventDevice *dev) -> Status;

    /// replace_with()'s two halves. release() lets go of the device in
    /// service: the handler is taken out and the device detached, or the
    /// CPU leaves its group. adopt() puts |dev| into service in Oneshot mode,
    /// with the handler of an ordinary tick or as a member of |group|.
    auto release() -> void;
    auto adopt(ClockEventDevice &dev, TickBroadcast *group) -> Status;

    gktl::Canary m_canary;
    ClockEventDevice *m_clkdev;
    Mode m_mode;
    /// The group m_clkdev serves this CPU through, if it is a global device
    /// shared by several CPUs; null when m_clkdev is this CPU's own.
    TickBroadcast *m_group;
};

class TickDevice::Current: public mm::CpuLocal::Current<TickDevice> {
    using Self = TickDevice::Current;

  public:
    /// Override methods faster than the way through Current::get().*();
    FORCE_INLINE
    static auto is_mode_oneshot() -> bool {
        arch::AutoInterruptDisabler guard;
        return mm::CpuLocal::read(CURRENT_INSTANCE.m_mode) == Mode::Oneshot;
    }

    FORCE_INLINE
    static auto is_mode_periodic() -> bool {
        arch::AutoInterruptDisabler guard;
        return mm::CpuLocal::read(CURRENT_INSTANCE.m_mode) == Mode::Periodic;
    }
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_DEVICE_HPP