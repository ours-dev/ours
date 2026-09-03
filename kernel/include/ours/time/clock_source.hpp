/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_SOURCE_HPP
#define OURS_TIME_CLOCK_SOURCE_HPP 1

#include <gktl/canary.hpp>
#include <gktl/vtable_packer.hpp>
#include <ours/status.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/types.hpp>
#include <ours/types.hpp>
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

    struct Vtable {
        template <auto Member>
        using Packer = gktl::VtablePacker<Self, Member>;

        auto (*read)(Self *) -> usize;
        auto (*enable)(Self *) -> Status;
        auto (*disable)(Self *) -> Status;
        auto (*suspend)(Self *) -> Status;
        auto (*resume)(Self *) -> Status;
    };

    static auto enroll(Self *self, usize hz) -> Status;

    /// Read cycle value.
    auto read() -> usize {
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

  protected:
    static auto enqueue_source(Self *self) -> void;
    static auto select_best_source(bool skipcur) -> Self *;

    gktl::Canary m_canary;
    Vtable const *m_vtable;
    char const *m_name;
    Flags m_flags;

    // Tick to NS.
    u32 numerator_;
    u32 denominator_;

    u32 m_priority;
    u32 m_freq_khz;

    usize m_max_cycle;
    usize m_last_cycle;

    static Self *CURRENT_CLKSRC;

  private:
    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);
    USTL_DECLARE_LIST(Self, List, ManagedOption);

    static List REGISTERED_SOURCES;
    static sync::Mutex REGISTERED_SOURCES_MUTEX;
};

/// Default clock source which advance by frequency OURS_CONFIG_HZ.
class ClockSource::Default {
    using Self = ClockSource::Default;

  public:
    static auto init() -> void;

    static auto read() -> usize;

    static auto get() -> ClockSource & {
        return *CLKEVT;
    }

  private:
    friend class TickSched;
    friend class TickDevice;

    static auto advance_ticks(Ticks num_ticks) -> void;

    static auto advance_time(Duration delta) -> void;

    static inline ClockSource *CLKEVT;

    /// The CPU which is responsible to update jiffies.
    static inline CpuNum CPUNUM;
    static inline sync::SpinLock SPINLOCK;

    /// One unit is equal to 1/HZ second.
    static inline Ticks TICKS TA_GUARDED(SPINLOCK);
    static inline Ticks LAST_TICKS;
    static inline TimePoint<> NEXT_PERIOD;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_SOURCE_HPP