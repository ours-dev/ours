/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_EVENT_DEVICE_HPP
#define OURS_TIME_CLOCK_EVENT_DEVICE_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>

#include <concepts>

#include <gktl/canary.hpp>
#include <gktl/vtable_packer.hpp>
#include <mtl/ratio.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/function.hpp>
#include <ustl/mem/container_of.hpp>
#include <ustl/traits/invoke_args.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/move.hpp>
#include <ustl/util/type_list.hpp>

namespace ours::time {
class ClockEventDevice {
    using Self = ClockEventDevice;

  public:
    /// Runs in the device's interrupt handler, so it must not allocate; see
    /// Timer::Handler. Move-only, so a handler changes hands through
    /// set_event_handler() and take_event_handler() and is never duplicated.
    using Handler = ustl::function::Function<auto(Self *)->void>;

    enum class Mode : u8 {
        Detached,
        Shutdown,
        Periodic,
        Oneshot,
        OneshotStopped,
    };

    enum class Feature : u8 {
        None,
        Periodic = 0_bit,
        Oneshot = 1_bit,
        TimePoint = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Feature);

    /// What program_event() does with a deadline it cannot meet -- one that
    /// has already passed, or one the device refuses as too close.
    ///
    /// The two callers want opposite things from such a deadline, which is
    /// why this is the caller's choice and not the device's.
    enum class LateDeadline : u8 {
        /// Arm the soonest interrupt the hardware can deliver instead. For a
        /// caller whose only interest in a missed deadline is to be
        /// interrupted about it as soon as possible: the timer queue, which
        /// then runs whatever has expired.
        Force,

        /// Arm nothing and return Status::TimeOut. For a caller that has
        /// something cheaper to do with a missed deadline than take an
        /// interrupt for it: the emulated periodic tick, which counts the
        /// missed period in place and moves on to the next one.
        Reject,
    };

    struct Vtable;

    /// How good a tick source this device is, relative to the others on the
    /// same CPU; TickDevice::check_new_device() picks the highest. The x86
    /// drivers rate the PIT 110, the local APIC timer 100 and the HPET 120.
    /// Only the order matters, and only among devices that may compete for the
    /// same CPU: a per-CPU device wins over a global one regardless of rating,
    /// see TickDevice::check_preferred().
    using Rating = u32;

    /// Registers |self| as a device whose counter ticks at |freq| Hz and can
    /// be programmed between |min_delta| and |max_delta| ticks ahead, then
    /// offers it to this CPU's TickDevice.
    ///
    /// There is no defaulted form. An earlier one filled in (HZ, 128, HZ),
    /// which failed its own min < max assertion and, worse, told the device
    /// that one tick was 10 ms -- every delta it was ever asked for came out
    /// a hundred thousand times too long.
    ///
    /// Nothing is returned: there is nothing a driver could do with an answer.
    /// Whether this CPU takes the device is the TickDevice's choice (it may
    /// keep a better one), a device that lacks a method its features claim is a
    /// driver bug, refused with a message and an assertion, and a device that
    /// fails to start is logged here.
    static auto enroll(Self &self, usize freq, Ticks min_delta, Ticks max_delta) -> void;

    /// |cpumask| is the set of CPUs this device can interrupt; a null mask
    /// means "any" and is replaced with cpu_possible_mask() at enrollment.
    /// A per-CPU device (the local APIC timer) must pass a mask holding
    /// exactly its own CPU -- that is what marks it per-CPU to TickDevice.
    ClockEventDevice(char const *name, Vtable const *, Feature, Rating, CpuMask const *);

    ~ClockEventDevice() = default;

    /// Configure the clock event device with the specified frequency and delta ticks.
    auto configure(usize frequency, Ticks min_delta, Ticks max_delta) -> void;

    /// Arms the device to interrupt at |expiry|. |late| says what happens when
    /// that cannot be done as asked; see LateDeadline. Defaulted to Force
    /// because that is what every caller but one wants, and the one exception
    /// (TickDevice::handle_periodic_event) has to say so where it is written.
    auto program_event(TimePoint<> expiry, LateDeadline late = LateDeadline::Force) -> Status;

    /// Switch the clock event device to a new state and modify its work mode.
    auto switch_mode(Mode target) -> Status;

    FORCE_INLINE
    auto set_event_handler(Handler handler) -> void {
        m_event_handler = ustl::move(handler);
    }

    /// Moves the handler out and leaves the device with none, which is what
    /// detaching a device needs anyway: a straggling interrupt from the
    /// outgoing device then finds nothing to run. A getter that copied would
    /// need a copyable Handler, and one that is copyable cannot hold a
    /// move-only target.
    FORCE_INLINE
    auto take_event_handler() -> Handler {
        return ustl::move(m_event_handler);
    }

    FORCE_INLINE
    auto set_irqnum(VIrqNum irqnum) -> void {
        m_irqnum = irqnum;
    }

    FORCE_INLINE
    auto irqnum() const -> VIrqNum {
        return m_irqnum;
    }

    FORCE_INLINE
    auto cpumask() const -> CpuMask const & {
        return *m_cpumask;
    }

    /// For devices whose mask is only known once they are on their CPU.
    FORCE_INLINE
    auto set_cpumask(CpuMask const *cpumask) -> void {
        m_cpumask = cpumask;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto rating() const -> Rating {
        return m_rating;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto name() const -> char const * {
        return m_name;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto has_feature(Feature feature) const -> bool {
        return !!(m_feature & feature);
    }

    [[nodiscard]]
    FORCE_INLINE
    auto is_mode_detached() const -> bool {
        return m_mode == Mode::Detached;
    }

    FORCE_INLINE
    auto is_mode_oneshot() const -> bool {
        return m_mode == Mode::Oneshot;
    }

    FORCE_INLINE
    auto is_mode_oneshot_stopped() const -> bool {
        return m_mode == Mode::OneshotStopped;
    }

    FORCE_INLINE
    auto is_mode_periodic() const -> bool {
        return m_mode == Mode::Periodic;
    }

    FORCE_INLINE
    auto handle_event() -> void {
        if (m_event_handler) {
            m_event_handler(this);
        }
    }

    FORCE_INLINE
    auto next_event() const -> TimePoint<> {
        return m_next_event;
    }

  protected:
    friend class TickDevice;

    /// Re-arms the device as soon as it is willing to be armed, after
    /// program_event() was refused. See the definition for why.
    auto program_min_delta() -> Status;

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

    FORCE_INLINE
    auto ticks_to_time(Ticks ticks) -> Duration {
        return Duration(m_scaler.scale(ticks));
    }

    FORCE_INLINE
    auto time_to_ticks(Duration duration) -> Ticks {
        return m_scaler.inverse().scale(duration.count());
    }

    gktl::Canary m_canary;
    Vtable const *m_vtable;
    char const *m_name;
    Mode m_mode;
    Feature m_feature;
    Rating m_rating;
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

namespace priv {
    /// Assembles a Vtable for |Driver| by asking, slot by slot, whether the
    /// driver defines the matching method.
    ///
    /// This replaces a table written out by hand in every driver. That was
    /// wrong in three ways beyond being tedious. Every driver had to list all
    /// nine slots including the `nullptr`s, so the file said "this device
    /// does not support suspend" nine times over instead of just not
    /// mentioning it. Adding a slot to Vtable did not break any driver --
    /// designated initialisers value-initialise what is not listed -- it
    /// silently left the new slot null in all of them, which is worse than a
    /// compile error. And the pairing of slot name to method name was written
    /// out by hand while several slots share the signature
    /// `(ClockEventDevice *) -> Status`, so putting resume() in the suspend
    /// slot type-checked.
    ///
    /// The detection is in two steps on purpose. First, does a member of that
    /// name exist at all; then, a static_assert that it has the right
    /// signature. Testing callability alone would silently drop a method
    /// whose return type had drifted, which is the same silent-null failure
    /// this is meant to remove.
    ///
    /// Two things a driver has to respect: the methods must be public, since
    /// this is not a friend of the driver, and none of these names may be
    /// overloaded, since `&Driver::name` has to name exactly one function.
    template <typename Driver, auto Member>
    consteval auto clkevt_vtable() -> ClockEventDevice::Vtable {
        using Vtable = ClockEventDevice::Vtable;
        using Packer = typename Vtable::template Packer<Member>;
        using Device = ClockEventDevice;

        static_assert(
            requires { &Driver::set_next_event_tick; } ||
                requires { &Driver::set_next_event_time; },
            "a clock event device must define set_next_event_tick or "
            "set_next_event_time; without one it can never be armed");

        Vtable vtable{};

        if constexpr (requires { &Driver::set_next_event_tick; }) {
            static_assert(
                requires(Driver *driver, Ticks ticks) {
                    { driver->set_next_event_tick(ticks) } -> std::same_as<Status>;
                }, "set_next_event_tick must be `auto set_next_event_tick(Ticks) -> Status`");
            vtable.set_next_event_tick = Packer::template pack<&Driver::set_next_event_tick>();
        }

        if constexpr (requires { &Driver::set_next_event_time; }) {
            static_assert(
                requires(Driver *driver, TimePoint<> deadline) {
                    { driver->set_next_event_time(deadline) } -> std::same_as<Status>;
                }, "set_next_event_time must be `auto set_next_event_time(TimePoint<>) -> Status`");
            vtable.set_next_event_time = Packer::template pack<&Driver::set_next_event_time>();
        }

        if constexpr (requires { &Driver::set_mode_periodic; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->set_mode_periodic() } -> std::same_as<Status>;
                }, "set_mode_periodic must be `auto set_mode_periodic() -> Status`");
            vtable.set_mode_periodic = Packer::template pack<&Driver::set_mode_periodic>();
        }

        if constexpr (requires { &Driver::set_mode_oneshot; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->set_mode_oneshot() } -> std::same_as<Status>;
                }, "set_mode_oneshot must be `auto set_mode_oneshot() -> Status`");
            vtable.set_mode_oneshot = Packer::template pack<&Driver::set_mode_oneshot>();
        }

        if constexpr (requires { &Driver::stop_oneshot; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->stop_oneshot() } -> std::same_as<Status>;
                }, "stop_oneshot must be `auto stop_oneshot() -> Status`");
            vtable.stop_oneshot = Packer::template pack<&Driver::stop_oneshot>();
        }

        if constexpr (requires { &Driver::resume; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->resume() } -> std::same_as<Status>;
                }, "resume must be `auto resume() -> Status`");
            vtable.resume = Packer::template pack<&Driver::resume>();
        }

        if constexpr (requires { &Driver::suspend; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->suspend() } -> std::same_as<Status>;
                }, "suspend must be `auto suspend() -> Status`");
            vtable.suspend = Packer::template pack<&Driver::suspend>();
        }

        if constexpr (requires { &Driver::shutdown; }) {
            static_assert(
                requires(Driver *driver) {
                    { driver->shutdown() } -> std::same_as<void>;
                }, "shutdown must be `auto shutdown() -> void`");
            vtable.shutdown = Packer::template pack<&Driver::shutdown>();
        }

        if constexpr (requires { &Driver::broadcast; }) {
            static_assert(
                requires(Driver *driver, CpuMask const &targets) {
                    { driver->broadcast(targets) } -> std::same_as<void>;
                }, "broadcast must be `auto broadcast(CpuMask const &) -> void`");
            vtable.broadcast = Packer::template pack<&Driver::broadcast>();
        }

        static_cast<void>(sizeof(Device));
        return vtable;
    }
} // namespace priv

/// The operations table for |Driver|, assembled from whichever of the
/// operations that driver actually defines.
///
/// |Member| is the pointer to Driver's ClockEventDevice member, which is how
/// a call gets from the device back to its driver. A driver names itself and
/// that member once, and writes no table:
///
///     class HpetChannel {
///         ClockEventDevice m_clkevt;
///       public:
///         HpetChannel()
///             : m_clkevt("HPET", &CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>, ...) {}
///
///         auto set_next_event_tick(Ticks) -> Status;
///         auto set_mode_periodic() -> Status;
///         auto shutdown() -> void;
///         // Nothing else: an operation the driver does not define is an
///         // operation it does not support.
///     };
///
/// It lives out here rather than as a member of ClockEventDevice because a
/// static member could not be declared `constexpr` in-class -- Vtable is
/// still incomplete there -- and without that the table would be correct at
/// run time but not inspectable at compile time.
template <typename Driver, auto Member>
inline constexpr ClockEventDevice::Vtable CLKEVT_VTABLE_OF = priv::clkevt_vtable<Driver, Member>();

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_EVENT_DEVICE_HPP