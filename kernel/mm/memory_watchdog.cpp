/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/boot/options.hpp>
#include <ours/init.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/memory_watchdog.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/task/thread.hpp>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::mm {
namespace {
    constexpr usize FRAMES_PER_MIB = 1_mb / PAGE_SIZE;
    constexpr usize NO_BOUND = ~usize{0};

    /// How far |free| frames are below the upper edge of |pressure|'s
    /// bounds, which is where reclaim at that pressure aims; zero when they
    /// are not below it.
    auto reclaim_distance(MemoryPressureLevels const &levels, MemoryPressure const pressure,
                          usize const free) -> usize {
        auto const upper = levels.bounds_of(pressure).upper;
        if (upper > free) {
            return upper - free;
        }
        return 0;
    }
} // namespace

auto to_string(MemoryPressure const pressure) -> char const * {
    switch (pressure) {
        case MemoryPressure::Normal:
            return "Normal";
        case MemoryPressure::Warning:
            return "Warning";
        case MemoryPressure::Critical:
            return "Critical";
        case MemoryPressure::OutOfMemory:
            return "OutOfMemory";
    }
    return "?";
}

auto MemoryPressureLevels::covering(usize const held) const -> MemoryPressureLevels {
    // These levels nest, and the raise keeps every gap and only lifts the
    // lowest level further above the debounce, so the result nests too.
    // delay_level() is the OutOfMemory level less the debounce, and it has
    // to be strictly above |held|. In every build: levels that wrapped would
    // not nest, and nothing downstream checks again. Boot levels and a
    // machine's floors are far from the top of the range.
    ASSERT(held < NO_BOUND - m_debounce, "{} frames held is too many to cover", held);
    auto const needed = held + m_debounce + 1;
    if (m_out_of_memory >= needed) {
        return *this;
    }
    auto const raise = needed - m_out_of_memory;
    ASSERT(raise <= NO_BOUND - m_debounce - m_warning,
           "levels with Warning at {} cannot be raised by {}", m_warning, raise);
    return {m_out_of_memory + raise, m_critical + raise, m_warning + raise, m_debounce,
            m_hysteresis};
}

auto MemoryPressureLevels::cover_zone_floors() const -> CoveredLevels {
    auto const held = zone_floor_frame_count();
    return CoveredLevels{covering(held), held};
}

auto MemoryPressureLevels::make(usize const out_of_memory, usize const critical,
                                usize const warning, usize const debounce,
                                Duration const hysteresis)
    -> ustl::Result<MemoryPressureLevels, Status> {
    if (!nest(out_of_memory, critical, warning, debounce)) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(MemoryPressureLevels{out_of_memory, critical, warning, debounce, hysteresis});
}

auto MemoryPressureLevels::nest(usize const out_of_memory, usize const critical,
                                usize const warning, usize const debounce) -> bool {
    // Differences, not sums: a sum of two large levels wraps and would let
    // levels that do not nest through. The last term keeps the widest bound
    // bounds_of() gives, the warning level plus the debounce, from wrapping.
    return out_of_memory > debounce && critical > out_of_memory &&
           critical - out_of_memory > debounce && warning > critical &&
           warning - critical > debounce && debounce <= NO_BOUND - warning;
}

auto MemoryPressureLevels::pressure_at(usize const free) const -> MemoryPressure {
    if (free < m_out_of_memory) {
        return MemoryPressure::OutOfMemory;
    }
    if (free < m_critical) {
        return MemoryPressure::Critical;
    }
    if (free < m_warning) {
        return MemoryPressure::Warning;
    }
    return MemoryPressure::Normal;
}

auto MemoryPressureLevels::bounds_of(MemoryPressure const pressure) const -> Bounds {
    // The subtractions below are unsigned; levels that nest never wrap them.
    switch (pressure) {
        case MemoryPressure::Normal:
            return {.lower = m_warning - m_debounce, .upper = NO_BOUND};
        case MemoryPressure::Warning:
            return {.lower = m_critical - m_debounce, .upper = m_warning + m_debounce};
        case MemoryPressure::Critical:
            return {.lower = m_out_of_memory - m_debounce, .upper = m_critical + m_debounce};
        case MemoryPressure::OutOfMemory:
            return {.lower = 0, .upper = m_out_of_memory + m_debounce};
    }
    return {.lower = 0, .upper = NO_BOUND};
}

auto MemoryPressureLevels::delay_level() const -> usize {
    return m_out_of_memory - m_debounce;
}

auto MemoryWatchdog::get() -> Self & {
    static Self WATCHDOG{MemoryReclaimer::get()};
    return WATCHDOG;
}

auto MemoryWatchdog::start(CoveredLevels const &levels) -> Status {
    // Set before the thread exists, which looks at them first thing; taken
    // back if it never runs, so that "levels set" keeps meaning "watchdog
    // running" (set_levels() asserts on it).
    {
        Guard guard{m_lock};
        m_levels = levels.levels();
    }
    auto const unstart = [this] {
        Guard guard{m_lock};
        m_levels = ustl::NONE;
    };

    // Thread::spawn() takes a plain function: this one only finds the
    // watchdog it was handed and runs it.
    auto *const thread = task::Thread::spawn(
        "memory-watchdog", [](void *const self) -> i32 { return static_cast<Self *>(self)->run(); },
        this, task::DEFAULT_PRIORITY);
    if (thread == nullptr) {
        unstart();
        return Status::OutOfMem;
    }
    auto _ = thread->detach();
    auto const status = thread->resume();
    if (status != Status::Ok) {
        unstart();
    }
    return status;
}

auto MemoryWatchdog::levels() const -> ustl::Option<MemoryPressureLevels> {
    Guard guard{m_lock};
    return m_levels;
}

auto MemoryWatchdog::set_levels(CoveredLevels const &levels) -> void {
    {
        Guard guard{m_lock};
        // Levels mean a running watchdog (levels()): only what start() began
        // may be moved.
        DEBUG_ASSERT(m_levels.is_some(), "the watchdog's levels are set before it was started");
        m_levels = levels.levels();
        m_skip_hysteresis = true;
    }
    m_signal.signal();
}

auto MemoryWatchdog::run() -> i32 {
    while (true) {
        if (auto const deadline = look(); deadline.is_some()) {
            auto _ = m_signal.wait(deadline.value());
        }
    }
}

auto MemoryWatchdog::look() -> ustl::Option<time::Deadline> {
    // The count is taken under the lock. The walk sleeps in the page owner's
    // lock, and m_lock guards only the levels and the declaration, so it is
    // dropped for the walk. Hold and set_levels() can run then. What is
    // declared below reads the levels and the free memory again.
    usize frames = 0;
    {
        Guard guard{m_lock};
        // The thread is started by start(), which sets the levels first.
        DEBUG_ASSERT(m_levels.is_some(), "the watchdog looks before it was started");
        auto const free = approx_free_frame_count();
        auto const found = m_levels->pressure_at(free);
        if (found == MemoryPressure::OutOfMemory) {
            // Nothing waits for a thread here: the allocations that could wait
            // are about to be refused, so what can be given back is given back
            // first, on this thread.
            frames = reclaim_distance(*m_levels, found, free);
        }
    }
    if (frames != 0) {
        // This walk has its own count, in place of any ask the thread has
        // not taken (reclaim_superseding()).
        auto const freed = m_reclaimer.reclaim_superseding(frames);
        if (freed != 0) {
            log::info("mm: out of memory: gave back {} frames before refusing anything", freed);
        }
    }

    Guard guard{m_lock};
    auto free = approx_free_frame_count();
    auto found = m_levels->pressure_at(free);
    if (found == MemoryPressure::Warning || found == MemoryPressure::Critical) {
        m_reclaimer.request(reclaim_distance(*m_levels, found, free));
    }
    auto const now = time::SteadyClock::now();

    auto deadline = time::Deadline::infinite();
    auto declare = !m_declared || m_skip_hysteresis || found > m_declared->pressure;
    if (!declare && found < m_declared->pressure) {
        if (now - m_declared->at >= m_levels->hysteresis()) {
            declare = true;
        } else {
            deadline = time::Deadline::at(m_declared->at + m_levels->hysteresis());
        }
    }
    m_skip_hysteresis = false;
    if (declare && (!m_declared || found != m_declared->pressure)) {
        log::info("mm: memory pressure {}, {} MiB free", to_string(found), free / FRAMES_PER_MIB);
        if (found == MemoryPressure::OutOfMemory) {
            log::warn("mm: out of memory, and nothing to reclaim: allocations that could wait "
                      "are refused");
        }
        m_declared = ustl::some(Declaration{.pressure = found, .at = now});
        m_pressure.store(found, std::memory_order_release);
    }

    // At OutOfMemory there is nothing to wait for: the waiters are refused,
    // and the frames below the delay level stay the kernel's.
    auto delayed = DelayedAllocations::Wait;
    if (found == MemoryPressure::OutOfMemory) {
        delayed = DelayedAllocations::Refuse;
    }
    auto const bounds = m_levels->bounds_of(found);
    if (!set_free_memory_signal(bounds.lower, bounds.upper, m_levels->delay_level(), &m_signal,
                                delayed)) {
        return ustl::NONE;
    }
    return ustl::some(deadline);
}

namespace {
    /// The init hook takes a plain function, as the thread does.
    INIT_CODE
    auto start_memory_watchdog() -> void {
        // The levels the kernel.oom.* boot options give. Levels that do not
        // nest are refused here and reported, not repaired: there is then no
        // watchdog.
        auto const &options = boot::options();
        auto const given = MemoryPressureLevels::make(
            usize{options.oom_out_of_memory_mb()} * FRAMES_PER_MIB,
            usize{options.oom_critical_mb()} * FRAMES_PER_MIB,
            usize{options.oom_warning_mb()} * FRAMES_PER_MIB,
            usize{options.oom_debounce_mb()} * FRAMES_PER_MIB,
            duration_cast<Duration>(Seconds(options.oom_hysteresis_seconds())));
        if (!given) {
            log::error("mm: the kernel.oom levels {}/{}/{} MiB with a debounce of {} MiB do not "
                       "nest; no memory watchdog",
                       options.oom_out_of_memory_mb(), options.oom_critical_mb(),
                       options.oom_warning_mb(), options.oom_debounce_mb());
            return;
        }

        // Raised where this machine's zone floors need it, and said once,
        // here, because the levels the log and the command line show are
        // then not the ones in force.
        auto const covered = given->cover_zone_floors();
        auto const &levels = covered.levels();
        if (levels.out_of_memory() > given->out_of_memory()) {
            log::info("mm: kernel.oom levels raised {} MiB: the zones' floors hold {} MiB",
                      (levels.out_of_memory() - given->out_of_memory() + FRAMES_PER_MIB - 1) /
                          FRAMES_PER_MIB,
                      covered.held() / FRAMES_PER_MIB);
        }
        if (auto const status = MemoryWatchdog::get().start(covered); Status::Ok != status) {
            log::error("mm: no thread for the memory watchdog: {}", to_string(status));
        }
    }
    // Once threads can be made: the DPC workers start on Threading itself.
    GKTL_INIT_HOOK(MemoryWatchdog, start_memory_watchdog, gktl::InitLevel::Threading + 1);
} // namespace

} // namespace ours::mm
