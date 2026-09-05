/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_DEVICE_HPP
#define OURS_TIME_TICK_DEVICE_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock_event_device.hpp>

namespace ours::time {
/// Each CPU's view of "the device that interrupts me": a TickDevice is the
/// per-CPU binding between the tick layer above (TickSched, TimerQueue) and
/// whichever ClockEventDevice is currently best for this CPU.
///
/// It owns three decisions, and nothing else does:
///
///  * Which device this CPU uses. Drivers do not pick; they enroll, and
///    check_new_device() decides per CPU -- a per-CPU device beats a global
///    one, oneshot-capable beats periodic-only, then rating. This is
///    tick_check_new_device() in Linux's kernel/time/tick-common.c, and the
///    rules are written out on check_percpu() and check_preferred().
///
///  * How the device is driven. In Periodic mode the device fires every
///    1/HZ and handle_periodic_event() is its handler; a device that cannot
///    do periodic natively is re-armed one tick ahead from that handler. In
///    Oneshot mode the device is armed for exactly the next deadline the
///    timer queue asks for through program_event(). Switching devices keeps
///    the mode, the handler and the pending deadline -- tick_setup_device().
///
///  * When the device is silent. In Oneshot mode a program_event(INFINITE)
///    parks the device in OneshotStopped, and the next deadline wakes it.
///
/// The binding is per CPU because a global device (PIT, HPET) can only
/// interrupt one CPU at a time, so every other CPU needs its own local APIC
/// timer; broadcast -- one global device waking sleeping CPUs by IPI -- is
/// the piece not built yet, which is why a device that loses the selection
/// is simply left aside rather than installed as the broadcast device.
class TickDevice {
    using Self = TickDevice;

  public:
    class Current;

    enum class Mode {
        Periodic,
        Oneshot,
    };

    TickDevice();

    ~TickDevice() = default;

    /// Programs next tick event with |expired|.
    ///
    /// Return Status::Ok on success, or an error code on failure.
    auto program_event(TimePoint<> expired) -> Status;

    /// Offers |new_clkdev| to this CPU. It is installed -- replacing the
    /// current device, if any -- when it can interrupt this CPU and is
    /// preferable to what is bound now; see check_percpu() and
    /// check_preferred() for what "preferable" means.
    ///
    /// Must run on the CPU it is deciding for, like Linux's
    /// tick_check_new_device(): a driver enrolls on the CPU whose tick it
    /// is offering to drive.
    ///
    /// Returns Status::Ok when |new_clkdev| was installed, Status::AlreadyBound
    /// when this CPU keeps the device it has, or the error that stopped the
    /// installation part-way.
    auto check_new_device(ClockEventDevice *new_clkdev) -> Status;

    /// Installs |new_clkdev| unconditionally: detaches the current device
    /// and moves the mode, handler and pending deadline over. What
    /// check_new_device() does once it has decided; exposed for callers
    /// that have made the decision themselves.
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

    /// Moves this CPU from Periodic to Oneshot mode: |handler| becomes the
    /// device's handler and the device is armed for now. Linux's
    /// tick_switch_to_oneshot().
    auto switch_to_oneshot(ClockEventDevice::Handler handler) -> Status;

  private:
    /// A device is per-CPU when its mask names exactly |cpu|. The local
    /// APIC timer sets such a mask on itself; PIT and HPET carry the whole
    /// possible mask.
    static auto is_percpu(ClockEventDevice const &dev, CpuNum cpu) -> bool;

    /// Whether |newdev| may serve |cpu| at all. Linux's tick_check_percpu().
    [[nodiscard]]
    auto check_percpu(ClockEventDevice const &newdev, CpuNum cpu) const -> bool;

    /// Whether |newdev| is better than the current device, given that both
    /// may serve this CPU. Linux's tick_check_preferred().
    [[nodiscard]]
    auto check_preferred(ClockEventDevice const &newdev) const -> bool;

    /// Puts |dev| into service for this CPU's current mode. Linux's
    /// tick_setup_periodic() and tick_setup_oneshot().
    auto setup_periodic(ClockEventDevice *dev) -> Status;
    auto setup_oneshot(ClockEventDevice *dev, ClockEventDevice::Handler handler,
                       TimePoint<> next_event) -> Status;

    gktl::Canary m_canary;
    ClockEventDevice *m_clkdev;
    Mode m_mode;
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