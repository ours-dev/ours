/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_WATCHDOG_HPP
#define OURS_MM_MEMORY_WATCHDOG_HPP 1

#include <ours/chrono.hpp>
#include <ours/lockdep.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
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

class CoveredLevels;

/// Where each pressure begins, in frames of free memory: below the warning
/// level it is Warning, below the critical level Critical, below the
/// out-of-memory level OutOfMemory. The watchdog looks again only once free
/// memory has gone the debounce past a level, and declares a lower pressure
/// only the hysteresis after its last change.
///
/// Every value nests: each level above the one below by more than the
/// debounce, and the lowest above the debounce, so each pressure has a range
/// of its own the debounce does not swallow. make() is the only way to build
/// one and refuses levels that do not; a bad command line is refused there,
/// by the watchdog's init hook, and reported, not repaired.
class MemoryPressureLevels {
  public:
    /// The free memory the PMM keeps quiet about at one pressure, in frames.
    struct Bounds {
        usize lower;
        usize upper;
    };

    /// These levels with the delay level above |held| frames: unchanged
    /// when it already is, and otherwise all three levels moved up by the
    /// same amount, so that each band keeps the width it was given and the
    /// levels still nest.
    ///
    /// |held| is the most free memory there can be while every zone refuses
    /// an ordinary request (zone_floor_frame_count()). With the delay level
    /// above it, every zone refusing means free memory below the delay
    /// level: the gate shuts and the watchdog is woken, and at OutOfMemory
    /// the waiters are refused. Below it, every zone could refuse while
    /// free memory stayed above the delay level, and a waiting request put
    /// off at a zone's Low would wait for a release that need never come.
    [[nodiscard]]
    auto covering(usize held) const -> MemoryPressureLevels;

    /// These levels covering this machine's zone floors, read here
    /// (zone_floor_frame_count()): the only way to the levels a watchdog runs
    /// with (CoveredLevels).
    [[nodiscard]]
    auto cover_zone_floors() const -> CoveredLevels;

    /// Levels that nest, or InvArgs when these do not.
    [[nodiscard]]
    static auto make(usize out_of_memory, usize critical, usize warning, usize debounce,
                     Duration hysteresis) -> ustl::Result<MemoryPressureLevels, Status>;

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
    /// Whether these levels nest; make() asks it before building one.
    [[nodiscard]]
    static auto nest(usize out_of_memory, usize critical, usize warning, usize debounce) -> bool;

    MemoryPressureLevels(usize out_of_memory, usize critical, usize warning, usize debounce,
                         Duration hysteresis)
        : m_out_of_memory(out_of_memory)
        , m_critical(critical)
        , m_warning(warning)
        , m_debounce(debounce)
        , m_hysteresis(hysteresis) {}

    usize m_out_of_memory;
    usize m_critical;
    usize m_warning;
    usize m_debounce;
    Duration m_hysteresis;
};

/// Levels whose delay level is above every frame the zones' floors can hold
/// (MemoryPressureLevels::covering()): with them, every zone refusing an
/// ordinary request means free memory below the delay level, so the gate
/// shuts and the watchdog is woken, and a request put off at a zone's Low has
/// something bound to end its wait. The only levels a watchdog takes
/// (MemoryWatchdog::start(), set_levels()), and made only by
/// MemoryPressureLevels::cover_zone_floors(), which reads the floors itself:
/// levels that skip the rule do not compile.
class CoveredLevels {
  public:
    [[nodiscard]]
    auto levels() const -> MemoryPressureLevels const & {
        return m_levels;
    }

    /// The frames the zones' floors hold, which the levels were raised above
    /// where they were not already.
    [[nodiscard]]
    auto held() const -> usize {
        return m_held;
    }

  private:
    friend class MemoryPressureLevels;

    CoveredLevels(MemoryPressureLevels const &levels, usize const held)
        : m_levels(levels), m_held(held) {}

    MemoryPressureLevels m_levels;
    usize m_held;
};

class MemoryReclaimer;

/// A kernel thread that watches how much memory is free, through the PMM's
/// free-memory signal (set_free_memory_signal()), and turns it into a
/// pressure. It sets the PMM's gate for the allocations that can wait: they
/// wait below the delay level until the watchdog has looked; and once it has
/// declared OutOfMemory, with nothing to reclaim, they are refused. Each
/// change of pressure is logged.
///
/// Started at boot with the levels the `kernel.oom.*` boot options give,
/// raised where the zones' floors need it.
class MemoryWatchdog {
    using Self = MemoryWatchdog;

  public:
    /// The one watchdog.
    static auto get() -> Self &;

    MemoryWatchdog(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Starts its thread, watching with |levels|. OutOfMem when there is no
    /// thread for it.
    auto start(CoveredLevels const &levels) -> Status;

    /// The pressure last declared.
    [[nodiscard]]
    auto pressure() const -> MemoryPressure {
        return m_pressure.load(std::memory_order_acquire);
    }

    /// The levels in use, or none before start().
    [[nodiscard]]
    auto levels() const -> ustl::Option<MemoryPressureLevels>;

    /// Replaces the levels of a started watchdog, and it looks again at once,
    /// without waiting for the hysteresis to declare a lower pressure. For tests,
    /// which move the levels to where free memory is, rather than free
    /// memory to where they are, and put back what levels() gave them.
    auto set_levels(CoveredLevels const &levels) -> void;

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
    /// None until start(): there are no levels to watch with before the
    /// boot options have been read.
    ustl::Option<MemoryPressureLevels> m_levels TA_GUARDED(m_lock);

    /// A declaration of pressure: what was declared, and when.
    struct Declaration {
        MemoryPressure pressure;
        time::SteadyClock::TimePoint at;
    };

    /// The pressure declared last and when; none before the first look.
    /// look()'s own, under m_lock like the rest of a look.
    ustl::Option<Declaration> m_declared TA_GUARDED(m_lock);

    /// set_levels() asks for the next look to declare what it finds, lower
    /// pressure or not: a test moves the levels and wants the answer now. An
    /// override of the next look's hysteresis, not part of the declaration:
    /// clearing m_declared instead would log an unchanged pressure again.
    bool m_skip_hysteresis TA_GUARDED(m_lock) = false;

    /// What pressure() answers, the declaration's pressure kept apart so that
    /// it can be read without m_lock: written by look() alone.
    std::atomic<MemoryPressure> m_pressure{MemoryPressure::Normal};
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_WATCHDOG_HPP
