/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_SOURCE_HPP
#define OURS_TIME_CLOCK_SOURCE_HPP 1

#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <gktl/vtable_packer.hpp>
#include <mtl/ratio.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::time {
class ClockSource {
    using Self = ClockSource;

  public:
    enum class Flags {
        None = 0,
        Enrolled = BIT(0),
        HpClock = BIT(1),
        Unstable = BIT(2),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    class Default;

    /// How good a time source this is, on Linux's clocksource::rating scale
    /// (jiffies 1, ACPI PM timer 200, HPET 250, TSC 300). enqueue_source()
    /// keeps the registered list in descending order of it, so
    /// select_best_source() only has to walk from the front.
    using Rating = u32;

    struct Vtable {
        template <auto Member>
        using Packer = gktl::VtablePacker<Self, Member>;

        auto (*read)(Self *) -> Ticks;
        auto (*enable)(Self *) -> Status;
        auto (*disable)(Self *) -> Status;
        auto (*suspend)(Self *) -> Status;
        auto (*resume)(Self *) -> Status;
    };

    /// Registers |self| as a source ticking at |hz|, and switches the kernel
    /// clocks to it if it is the best one enrolled so far.
    ///
    /// The frequency is the one thing a Clock needs from its source and the
    /// one thing the driver knows, so it is taken here and turned into the
    /// tick-to-nanosecond ratio once; nothing downstream sees a raw Hz.
    /// Requires a 64-bit counter: Clock reads it through an unsigned
    /// subtraction from a fixed anchor and never re-anchors, so a narrower
    /// counter would wrap under it.
    static auto enroll(Self *self, usize hz, Rating rating) -> Status;

    [[nodiscard]]
    auto name() const -> char const * {
        return m_name;
    }

    [[nodiscard]]
    auto freq_hz() const -> usize {
        return m_freq_hz;
    }

    /// Scales a count of this source's ticks to nanoseconds, exactly, through
    /// mtl::Ratio's 96-bit intermediate. This is why Clock can use an affine
    /// map instead of Linux's (mult, shift) pair: (mult, shift) is a 32x64
    /// product that stays exact only while the delta is bounded, which is
    /// what Linux's per-tick accumulation guarantees and Clock deliberately
    /// does not.
    [[nodiscard]]
    auto ticks_to_time(Ticks ticks) const -> Duration {
        return Duration(m_ticks_to_ns.scale(ticks));
    }

    /// The raw counter, in this source's own ticks.
    ///
    /// Unsigned, and deliberately so: these are hardware counters that wrap,
    /// and Clock reads them by subtracting a fixed anchor, which only gives
    /// the right answer across a wrap in unsigned arithmetic. (The review
    /// that led to this cleanup suggested making Ticks signed, like Zircon's
    /// zx_ticks_t, on the grounds that unsigned made `< 0` checks vacuous.
    /// Those two checks were dead code and have been removed instead; the
    /// wraparound argument is the one that survives.)
    auto read() -> Ticks {
        if (m_vtable->read) {
            return m_vtable->read(this);
        }

        return 0;
    }

    auto enable() -> Status {
        if (m_vtable->enable) {
            return m_vtable->enable(this);
        }
        return Status::Unsupported;
    }

    auto disable() -> Status {
        if (m_vtable->disable) {
            return m_vtable->disable(this);
        }
        return Status::Unsupported;
    }

    auto suspend() -> Status {
        if (m_vtable->suspend) {
            return m_vtable->suspend(this);
        }
        return Status::Unsupported;
    }

    auto resume() -> Status {
        if (m_vtable->resume) {
            return m_vtable->resume(this);
        }
        return Status::Unsupported;
    }

    ClockSource(char const *name, Vtable const *);

    ~ClockSource() = default;

  private:
    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);

    // Declared ahead of the methods below so their TA_REQUIRES can name it.
    static sync::Mutex REGISTERED_SOURCES_MUTEX;
    static List REGISTERED_SOURCES TA_GUARDED(REGISTERED_SOURCES_MUTEX);

  protected:
    TA_REQUIRES(REGISTERED_SOURCES_MUTEX)
    static auto enqueue_source(Self *self) -> void;

    TA_REQUIRES(REGISTERED_SOURCES_MUTEX)
    static auto select_best_source(bool skipcur) -> Self *;

    gktl::Canary m_canary;
    Vtable const *m_vtable;
    char const *m_name;
    Flags m_flags;

    /// Ticks to nanoseconds; set once by enroll() from |m_freq_hz|.
    mtl::Ratio<u32> m_ticks_to_ns;

    Rating m_rating;
    usize m_freq_hz;

    /// Neither is read yet. They are for the watchdog that has not been
    /// written: the largest delta this counter can represent before it wraps,
    /// and the last value seen, which is how Linux's clocksource_watchdog
    /// notices a source that has jumped and marks it Flags::Unstable.
    Ticks m_max_cycle;
    Ticks m_last_cycle;

    static Self *CURRENT_CLKSRC;
};

/// Jiffies: the software clock source that a periodic tick advances, Linux's
/// clocksource_jiffies. It is the fallback that exists before any hardware
/// counter has been found, and the yardstick the local APIC calibrates
/// against.
///
/// Only the counter half of it is live. read() and advance_ticks() work --
/// the periodic tick calls the latter -- but init() has no definition and
/// SOURCE is never filled, so this is not yet enrolled as a ClockSource and
/// get() must not be called. Until it is, SteadyClock has no source, and
/// therefore reads zero, until the HPET enrolls at InitLevel::Irq + 2.
class ClockSource::Default {
    using Self = ClockSource::Default;

  public:
    static auto init() -> void;

    static auto read() -> Ticks;

    /// Not usable yet: init() is unimplemented, so SOURCE is null.
    static auto get() -> ClockSource & {
        DEBUG_ASSERT(SOURCE, "The jiffies clock source has not been built yet");
        return *SOURCE;
    }

  private:
    friend class TickSched;
    friend class TickDevice;

    static auto advance_ticks(Ticks num_ticks) -> void;

    static auto advance_time(Duration delta) -> void;

    static inline ClockSource *SOURCE;

    /// The one CPU that advances jiffies: Linux's tick_do_timer_cpu. Nobody
    /// until the first CPU binds a tick device (TickDevice::rebind_device
    /// claims it), because only a CPU with a running device is sure to tick.
    static inline CpuNum CPUNUM = INVALID_CPU_NUM;

    /// One unit is equal to 1/HZ second.
    ///
    /// Atomic and lock-free, which Linux's jiffies also is (a plain volatile
    /// read of a word one CPU owns). It used to be guarded by a spin lock
    /// taken as `Guard<SpinLock, IrqSave>` on both sides, and that was a
    /// self-deadlock waiting to happen: the IrqSave option is inert (see
    /// TimerQueue's note), so the tick interrupt could land on this CPU while
    /// read() held the lock, and advance_ticks() would then spin forever on a
    /// lock held by the code it interrupted. The window is a handful of
    /// instructions, but the local APIC calibration polls read() in a tight
    /// loop for tens of milliseconds, which is exactly how you find one.
    ///
    /// Relaxed ordering is enough: this counter carries no data with it, and
    /// only ClockSource::Default::CPUNUM ever writes it.
    static inline std::atomic<Ticks> TICKS;
    static inline Ticks LAST_TICKS;
    static inline TimePoint<> NEXT_PERIOD;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_SOURCE_HPP