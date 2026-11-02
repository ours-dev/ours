/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_WATCHDOG_HPP
#define OURS_MM_MEMORY_WATCHDOG_HPP 1

#include <ours/lockdep.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>
#include <ours/types.hpp>

#include <atomic>

#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace ours::mm {

/// How short free memory is, from the least short to the most. Ordered, so
/// that a greater pressure compares greater.
enum class MemoryPressure : u8 {
    Normal,
    Warning,
    Critical,
    OutOfMemory,
};

[[nodiscard]]
auto to_string(MemoryPressure pressure) -> char const *;

/// Where each pressure begins, in frames of free memory: below the warning
/// level it is Warning, below the critical level Critical, below the
/// out-of-memory level OutOfMemory. The watchdog looks again only once free
/// memory has gone the debounce past a level, and declares a lower pressure
/// only the hysteresis after its last change.
///
/// make() is the way to build one that nests. from_boot_options() can be
/// otherwise, and start() then refuses it: a bad command line is reported,
/// not subtracted.
class MemoryPressureLevels {
  public:
    /// The free memory the PMM keeps quiet about at one pressure, in frames.
    struct Bounds {
        usize lower;
        usize upper;
    };

    /// The levels the `kernel.oom.*` boot options give. Not necessarily valid.
    [[nodiscard]]
    static auto from_boot_options() -> MemoryPressureLevels;

    /// A value whose levels nest. InvArgs when they do not.
    [[nodiscard]]
    static auto make(usize out_of_memory, usize critical, usize warning, usize debounce,
                     Duration hysteresis) -> ustl::Result<MemoryPressureLevels, Status>;

    /// Each level above the one before by more than the debounce, and the
    /// lowest above it too: then every pressure has a range of its own that
    /// the debounce does not swallow.
    [[nodiscard]]
    auto is_valid() const -> bool;

    /// The pressure at |free| frames of free memory.
    [[nodiscard]]
    auto pressure_at(usize free) const -> MemoryPressure;

    /// The range of |pressure| widened by the debounce on both sides, so
    /// that free memory moving to and fro across a level by a few frames, as
    /// every allocation and free makes it, does not wake the watchdog.
    [[nodiscard]]
    auto bounds_of(MemoryPressure pressure) const -> Bounds;

    /// Where the PMM's gate starts delaying the allocations that can wait:
    /// the debounce below the OutOfMemory level, which is where Critical's
    /// bounds end. The one allocation that takes free memory past it shuts
    /// the gate and wakes the watchdog together.
    [[nodiscard]]
    auto delay_level() const -> usize;

    [[nodiscard]]
    auto out_of_memory() const -> usize {
        return m_out_of_memory;
    }
    [[nodiscard]]
    auto critical() const -> usize {
        return m_critical;
    }
    [[nodiscard]]
    auto warning() const -> usize {
        return m_warning;
    }
    [[nodiscard]]
    auto debounce() const -> usize {
        return m_debounce;
    }
    [[nodiscard]]
    auto hysteresis() const -> Duration {
        return m_hysteresis;
    }

  private:
    friend class MemoryWatchdog;

    /// Not started: the watchdog replaces this in start().
    MemoryPressureLevels() = default;

    MemoryPressureLevels(usize out_of_memory, usize critical, usize warning, usize debounce,
                         Duration hysteresis)
        : m_out_of_memory(out_of_memory)
        , m_critical(critical)
        , m_warning(warning)
        , m_debounce(debounce)
        , m_hysteresis(hysteresis) {}

    usize m_out_of_memory = 0;
    usize m_critical = 0;
    usize m_warning = 0;
    usize m_debounce = 0;
    Duration m_hysteresis{};
};

class MemoryReclaimer;

/// A kernel thread that watches how much memory is free, through the PMM's
/// free-memory signal (set_free_memory_signal()), and turns it into a
/// pressure. It sets the PMM's gate for the allocations that can wait: they
/// wait below the delay level until the watchdog has looked; and once it has
/// declared OutOfMemory, with nothing to reclaim, they are refused. Each
/// change of pressure is logged.
///
/// Started at boot with the levels the `kernel.oom.*` boot options give.
class MemoryWatchdog {
    using Self = MemoryWatchdog;

  public:
    /// The one watchdog.
    static auto get() -> Self &;

    MemoryWatchdog(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Starts its thread, watching with |levels|. InvArgs for levels that
    /// are not is_valid(), OutOfMem when there is no thread for it.
    auto start(MemoryPressureLevels const &levels) -> Status;

    /// The pressure last declared.
    [[nodiscard]]
    auto pressure() const -> MemoryPressure {
        return m_pressure.load(std::memory_order_acquire);
    }

    /// The levels in use.
    [[nodiscard]]
    auto levels() const -> MemoryPressureLevels;

    /// Replaces the levels, and the watchdog looks again at once, without
    /// waiting for the hysteresis to declare a lower pressure. InvArgs for
    /// levels that are not is_valid(). For tests, which move the levels to
    /// where free memory is, rather than free memory to where they are.
    auto set_levels(MemoryPressureLevels const &levels) -> Status;

    /// While one is alive the watchdog arms nothing: for a test that drives
    /// the PMM's gate and signal by hand. The guard is what lockdep records.
    /// It is dropped before the signal, so the woken thread can take the mutex.
    class TA_SCOPED_CAPABILITY Hold {
      public:
        explicit Hold(MemoryWatchdog &watchdog) TA_ACQUIRE(watchdog.m_lock)
            : m_watchdog(watchdog), m_guard(watchdog.m_lock) {}

        ~Hold() TA_RELEASE() {
            m_guard.unlock();
            m_watchdog.m_signal.signal();
        }

        Hold(Hold const &) = delete;
        auto operator=(Hold const &) -> Hold & = delete;

      private:
        MemoryWatchdog &m_watchdog;
        Guard<sync::Mutex> m_guard;
    };

  private:
    explicit MemoryWatchdog(MemoryReclaimer &reclaimer) : m_reclaimer(reclaimer) {}

    /// The thread: looks, then sleeps until the PMM's signal, set_levels(),
    /// a Hold that goes or the end of a hysteresis wakes it.
    auto run() -> i32;

    /// One look at free memory: declares the pressure, if it is due, and
    /// arms the PMM for the next change. The deadline of the sleep that
    /// follows, or none when free memory moved past the bounds while it
    /// looked and it must look again at once.
    ///
    /// A higher pressure is declared at once, a lower one only once the
    /// hysteresis has passed since the last change, so that a load that comes
    /// and goes is not logged as a change each time. What the PMM is armed
    /// with follows the free memory found, not the pressure declared: the gate
    /// opens as soon as memory is back, the log waits.
    auto look() -> ustl::Option<time::Deadline>;

    /// What wakes the thread. Auto-unsignal, so each wakes it once.
    sync::Event m_signal{sync::Event::Options::AutoUnsignal};

    /// The reclaimer look() asks. Bound when the watchdog is made, so a look
    /// does not go and find the singleton.
    MemoryReclaimer &m_reclaimer;

    /// Held while the thread looks and arms the PMM, and for as long as a
    /// Hold lives; not while the thread sleeps.
    mutable sync::Mutex m_lock;
    MemoryPressureLevels m_levels TA_GUARDED(m_lock){};

    /// set_levels() asks for the next look to declare what it finds, lower
    /// pressure or not: a test moves the levels and wants the answer now.
    bool m_skip_hysteresis TA_GUARDED(m_lock) = false;

    /// What pressure() answers: written by look() alone.
    std::atomic<MemoryPressure> m_pressure{MemoryPressure::Normal};

    /// The pressure declared last and when, and whether anything has been
    /// yet: look()'s own, under m_lock like the rest of a look.
    MemoryPressure m_declared TA_GUARDED(m_lock) = MemoryPressure::Normal;
    time::SteadyClock::TimePoint m_declared_at TA_GUARDED(m_lock){};
    bool m_declared_once TA_GUARDED(m_lock) = false;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_WATCHDOG_HPP
