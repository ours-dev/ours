/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_DEVICE_HPP
#define OURS_TIME_TICK_DEVICE_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock_event_device.hpp>

namespace ours::time {
/// The lowest level tick device that provides consistent interfaces which
/// operates on the platform level clock event device.
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

    /// Check if the clock event device is valid and can be used. If it can be used
    /// and now we have one, the one will be replaced with the new clock event device.
    ///
    /// Return Status::Ok on success to replace, or an error code on failure.
    auto check_new_device(ClockEventDevice *new_clkdev) -> Status;

    /// Rebind the clock event device to a new one.
    auto rebind_device(ClockEventDevice *new_clkdev) -> Status;

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

    auto switch_to_oneshot(ClockEventDevice::Handler const &handler) -> void;

  private:
    gktl::Canary m_canary;
    ClockEventDevice *m_clkdev;
    Mode m_mode;
};

class TickDevice::Current: public mm::CpuLocal::Current<TickDevice> {
    using Self = TickDevice::Current;

  public:
    /// Override methods faster than the way through Current::get().*();
    FORCE_INLINE static auto is_mode_oneshot() -> bool {
        arch::AutoInterruptDisabler guard;
        return mm::CpuLocal::read(CURRENT_INSTANCE.m_mode) == Mode::Oneshot;
    }

    FORCE_INLINE static auto is_mode_periodic() -> bool {
        arch::AutoInterruptDisabler guard;
        return mm::CpuLocal::read(CURRENT_INSTANCE.m_mode) == Mode::Periodic;
    }
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_DEVICE_HPP