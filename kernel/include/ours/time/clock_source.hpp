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
        Enrolled = 0_bit,
        HpClock = 1_bit,
        Unstable = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    class Default;

    /// How good a time source this is, higher being better. The HPET, the
    /// only source enrolled today, rates itself 250, which leaves room above
    /// it for a better counter (a TSC) and below it for a worse one (an ACPI
    /// PM timer). enqueue_source() keeps the registered list in descending
    /// order of it, so select_best_source() only has to walk from the front.
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
    /// map instead of a (mult, shift) pair: (mult, shift) is a 32x64 product
    /// that stays exact only while the delta is bounded, which a per-tick
    /// accumulation would guarantee and Clock deliberately does not.
    [[nodiscard]]
    auto ticks_to_time(Ticks ticks) const -> Duration {
        return Duration(m_ticks_to_ns.scale(ticks));
    }

    [[nodiscard]]
    auto ns_numerator() const -> u32 {
        return m_ticks_to_ns.numerator();
    }

    [[nodiscard]]
    auto ns_denominator() const -> u32 {
        return m_ticks_to_ns.denominator();
    }

    /// The raw counter, in this source's own ticks.
    ///
    /// Unsigned, and deliberately so: these are hardware counters that wrap,
    /// and Clock reads them by subtracting a fixed anchor, which only gives
    /// the right answer across a wrap in unsigned arithmetic. (The review
    /// that led to this cleanup suggested making Ticks signed, on the
    /// grounds that unsigned made `< 0` checks vacuous.
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
    /// and the last value seen, which is how a watchdog notices a source
    /// that has jumped and marks it Flags::Unstable.
    Ticks m_max_cycle;
    Ticks m_last_cycle;

    static Self *CURRENT_CLKSRC;
};

/// Jiffies: the software clock source that a periodic tick advances. It is
/// the fallback that exists before any hardware counter has been found, and
/// the yardstick the local APIC calibrates against. Only a periodic tick
/// advances it: once its CPU runs one-shot, which has no tick, it stands
/// still, read by nothing.
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

    /// The CPU that advances jiffies (CPUNUM): the boot CPU, the first to
    /// bind a tick device. It never changes, so it is the one CPU that may
    /// not go offline (task::take_cpu_offline()). INVALID_CPU_NUM before any
    /// CPU has bound one.
    [[nodiscard]]
    static auto cpunum() -> CpuNum {
        return CPUNUM;
    }

  private:
    friend class TickDevice;

    static auto advance_ticks(Ticks num_ticks) -> void;

    static auto advance_time(Duration delta) -> void;

    static inline ClockSource *SOURCE;

    /// The one CPU that advances jiffies, so that a tick on every CPU does
    /// not advance it once per CPU. Nobody until the first CPU binds a tick
    /// device (TickDevice::rebind_device claims it), because only a CPU with
    /// a running device is sure to tick.
    static inline CpuNum CPUNUM = INVALID_CPU_NUM;

    /// One unit is equal to 1/HZ second.
    ///
    /// Atomic and lock-free: one CPU writes it and every reader only loads it.
    /// It used to be guarded by a spin lock taken as `Guard<SpinLock, IrqSave>`
    /// on both sides, and that was a self-deadlock waiting to happen: the
    /// IrqSave option is inert (see TimerQueue's note), so the tick interrupt
    /// could land on this CPU while read() held the lock, and advance_ticks()
    /// would then spin forever on a lock held by the code it interrupted. The
    /// window is a handful of instructions, but the local APIC calibration
    /// polls read() in a tight loop for tens of milliseconds, which is exactly
    /// how you find one.
    ///
    /// Relaxed ordering is enough: this counter carries no data with it, and
    /// only ClockSource::Default::CPUNUM ever writes it.
    static inline std::atomic<Ticks> TICKS;
    static inline Ticks LAST_TICKS;
    static inline TimePoint<> NEXT_PERIOD;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_SOURCE_HPP