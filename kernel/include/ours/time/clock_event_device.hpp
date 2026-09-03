/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_EVENT_DEVICE_HPP
#define OURS_TIME_CLOCK_EVENT_DEVICE_HPP 1

#include <gktl/canary.hpp>
#include <gktl/vtable_packer.hpp>
#include <mtl/ratio.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/macro_abi.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/mem/container_of.hpp>
#include <ustl/traits/invoke_args.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/type_list.hpp>

namespace ours::time {
class ClockEventDevice {
    using Self = ClockEventDevice;

  public:
    using Handler = ustl::function::Fn<auto(Self *)->void>;

    enum class Mode : u8 {
        Detached,
        Shutdown,
        Periodic,
        Oneshot,
        OneshotStopped,
    };

    enum class Feature : u8 {
        None,
        Periodic = BIT(0),
        Oneshot = BIT(1),
        TimePoint = BIT(2),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Feature);

    struct Vtable;

    static auto entroll(Self &self) -> Status;

    static auto entroll(Self &self, usize freq, Ticks min_delta, Ticks max_delta) -> Status;

    ClockEventDevice(char const *name, Vtable const *, Feature, CpuMask const *);

    ~ClockEventDevice() = default;

    /// Configure the clock event device with the specified frequency and delta ticks.
    auto configure(usize frequency, Ticks min_delta, Ticks max_delta) -> void;

    /// Program the clock event device to trigger an event at the specified time point.
    auto program_event(TimePoint<> expired) -> Status;

    /// Switch the clock event device to a new state and modify its work mode.
    auto switch_mode(Mode target) -> Status;

    FORCE_INLINE auto set_event_handler(Handler handler) -> void {
        m_event_handler = handler;
    }

    FORCE_INLINE auto get_event_handler() -> Handler {
        return m_event_handler;
    }

    FORCE_INLINE auto set_irqnum(VIrqNum irqnum) -> void {
        m_irqnum = irqnum;
    }

    FORCE_INLINE auto irqnum() const -> VIrqNum {
        return m_irqnum;
    }

    FORCE_INLINE auto cpumask() const -> CpuMask const & {
        return *m_cpumask;
    }

    FORCE_INLINE auto is_mode_oneshot() const -> bool {
        return m_mode == Mode::Oneshot;
    }

    FORCE_INLINE auto is_mode_oneshot_stopped() const -> bool {
        return m_mode == Mode::OneshotStopped;
    }

    FORCE_INLINE auto is_mode_periodic() const -> bool {
        return m_mode == Mode::Periodic;
    }

    FORCE_INLINE auto handle_event() -> void {
        if (m_event_handler) {
            m_event_handler(this);
        }
    }

    FORCE_INLINE auto next_event() const -> TimePoint<> {
        return m_next_event;
    }

  protected:
    friend class TickDevice;

    /// Wrappers to methods required in Vtable.
    auto set_next_event_tick(Ticks ticks) -> Status;
    auto set_next_event_time(TimePoint<> deadline) -> Status;

    auto set_mode_periodic() -> Status;
    auto set_mode_oneshot() -> Status;
    auto stop_oneshot() -> Status;

    auto resume() -> Status;
    auto suspend() -> Status;
    auto shutdown() -> Status;

    auto broadcast(CpuMask const &targets) -> Status;

    FORCE_INLINE auto ticks_to_time(Ticks ticks) -> Duration {
        return Duration(m_scaler.scale(ticks));
    }

    FORCE_INLINE auto time_to_ticks(Duration duration) -> Ticks {
        return m_scaler.inverse().scale(duration.count());
    }

    gktl::Canary m_canary;
    Vtable const *m_vtable;
    char const *m_name;
    Mode m_mode;
    Feature m_feature;
    VIrqNum m_irqnum;
    TimePoint<> m_next_event;

    /// Converts a duration to clock ticks.
    mtl::Ratio<u32> m_scaler;

    Ticks m_min_delta_ticks;
    Ticks m_max_delta_ticks;

    Duration m_min_delta; // ns
    Duration m_max_delta; // ns

    // TODO(SmallHuaZi): Instead of pointer of CpuMask, it is out of in
    // a bounded case that MAX_CPUS is too large.
    CpuMask const *m_cpumask;

    Handler m_event_handler;

  private:
    ustl::collections::intrusive::ListMemberHook<> m_enrolled_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_enrolled_hook, EnrolledListOption);

    USTL_DECLARE_LIST(Self, EnrolledList, EnrolledListOption);
    static inline EnrolledList GCED;
    static inline sync::Mutex GCED_MUTEX;
};

struct ClockEventDevice::Vtable {
    /// Helper meta function, which packs member methods into an Vtable instance.
    ///
    /// # Usage:
    ///     class YourDevice {
    ///         auto set_next_event_tick() -> void;
    ///         auto set_next_event_time() -> void;
    ///         ...
    ///         ClockEventDevice m_clkevt;
    ///         ...
    ///         using Packer = ClockEventDevice::Vtable::Packer<&YourDevice::m_clkevt>;
    ///         static ClockEventDevice::Vtable const VTABLE {
    ///             .set_next_event_tick = Packer::pack<&YourDevice::set_next_event_tick>();
    ///             .set_next_event_time = Packer::pack<&YourDevice::set_next_event_time>();
    ///             ...
    ///         };
    ///     };
    template <auto Member>
    using Packer = gktl::VtablePacker<Self, Member>;

    auto (*set_next_event_tick)(ClockEventDevice *, Ticks) -> Status;
    auto (*set_next_event_time)(ClockEventDevice *, TimePoint<>) -> Status;
    auto (*set_mode_periodic)(ClockEventDevice *) -> Status;
    auto (*set_mode_oneshot)(ClockEventDevice *) -> Status;
    auto (*stop_oneshot)(ClockEventDevice *) -> Status;
    auto (*resume)(ClockEventDevice *) -> Status;
    auto (*suspend)(ClockEventDevice *) -> Status;

    auto (*shutdown)(ClockEventDevice *) -> void;
    auto (*broadcast)(ClockEventDevice *, CpuMask const &) -> void;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_EVENT_DEVICE_HPP