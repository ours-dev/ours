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

auto MemoryPressureLevels::from_boot_options() -> MemoryPressureLevels {
    auto const &options = boot::options();
    return {usize{options.oom_out_of_memory_mb()} * FRAMES_PER_MIB,
            usize{options.oom_critical_mb()} * FRAMES_PER_MIB,
            usize{options.oom_warning_mb()} * FRAMES_PER_MIB,
            usize{options.oom_debounce_mb()} * FRAMES_PER_MIB,
            duration_cast<Duration>(Seconds(options.oom_hysteresis_seconds()))};
}

auto MemoryPressureLevels::make(usize const out_of_memory, usize const critical,
                                usize const warning, usize const debounce,
                                Duration const hysteresis)
    -> ustl::Result<MemoryPressureLevels, Status> {
    MemoryPressureLevels levels{out_of_memory, critical, warning, debounce, hysteresis};
    if (!levels.is_valid()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(levels);
}

auto MemoryPressureLevels::is_valid() const -> bool {
    return m_out_of_memory > m_debounce && m_critical > m_out_of_memory + m_debounce &&
           m_warning > m_critical + m_debounce;
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
    // The subtractions below are unsigned. An invalid value wraps them.
    DEBUG_ASSERT(is_valid(), "the levels do not nest");
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
    DEBUG_ASSERT(is_valid(), "the levels do not nest");
    return m_out_of_memory - m_debounce;
}

auto MemoryWatchdog::get() -> Self & {
    static Self WATCHDOG{MemoryReclaimer::get()};
    return WATCHDOG;
}

auto MemoryWatchdog::start(MemoryPressureLevels const &levels) -> Status {
    if (!levels.is_valid()) {
        return Status::InvArgs;
    }
    {
        Guard guard{m_lock};
        m_levels = levels;
    }

    // Thread::spawn() takes a plain function: this one only finds the
    // watchdog it was handed and runs it.
    auto *const thread = task::Thread::spawn(
        "memory-watchdog", [](void *const self) -> i32 { return static_cast<Self *>(self)->run(); },
        this, task::DEFAULT_PRIORITY);
    if (thread == nullptr) {
        return Status::OutOfMem;
    }
    auto _ = thread->detach();
    auto _ = thread->resume();
    return Status::Ok;
}

auto MemoryWatchdog::levels() const -> MemoryPressureLevels {
    Guard guard{m_lock};
    return m_levels;
}

auto MemoryWatchdog::set_levels(MemoryPressureLevels const &levels) -> Status {
    if (!levels.is_valid()) {
        return Status::InvArgs;
    }
    {
        Guard guard{m_lock};
        m_levels = levels;
        m_skip_hysteresis = true;
    }
    m_signal.signal();
    return Status::Ok;
}

auto MemoryWatchdog::run() -> i32 {
    while (true) {
        if (auto const deadline = look(); deadline.is_some()) {
            auto _ = m_signal.wait(deadline.value());
        }
    }
}

auto MemoryWatchdog::look() -> ustl::Option<time::Deadline> {
    Guard guard{m_lock};
    auto free = approx_free_frame_count();
    auto found = m_levels.pressure_at(free);

    // What is asked of the reclaimer is the distance to where this pressure
    // ends, the upper edge of its bounds: free memory there looks, to this,
    // like the pressure above.
    if (found == MemoryPressure::OutOfMemory) {
        // Nothing waits for a thread here: the allocations that could wait are
        // about to be refused, so what can be given back is given back first,
        // on this thread, and what is found afterwards is what is declared.
        auto const goal = m_levels.bounds_of(found).upper;
        if (auto const freed = m_reclaimer.reclaim(goal - free); freed != 0) {
            log::info("mm: out of memory: gave back {} frames before refusing anything", freed);
            free = approx_free_frame_count();
            found = m_levels.pressure_at(free);
        }
    }
    if (found == MemoryPressure::Warning || found == MemoryPressure::Critical) {
        m_reclaimer.request(m_levels.bounds_of(found).upper - free);
    }
    auto const now = time::SteadyClock::now();

    auto deadline = time::Deadline::infinite();
    auto declare = !m_declared_once || m_skip_hysteresis || found > m_declared;
    if (!declare && found < m_declared) {
        if (now - m_declared_at >= m_levels.hysteresis()) {
            declare = true;
        } else {
            deadline = time::Deadline::at(m_declared_at + m_levels.hysteresis());
        }
    }
    m_skip_hysteresis = false;
    if (declare && (!m_declared_once || found != m_declared)) {
        log::info("mm: memory pressure {}, {} MiB free", to_string(found), free / FRAMES_PER_MIB);
        if (found == MemoryPressure::OutOfMemory) {
            log::warn("mm: out of memory, and nothing to reclaim: allocations that could wait "
                      "are refused");
        }
        m_declared = found;
        m_declared_at = now;
        m_declared_once = true;
        m_pressure.store(found, std::memory_order_release);
    }

    // At OutOfMemory there is nothing to wait for: the waiters are refused,
    // and the frames below the delay level stay the kernel's.
    auto delayed = DelayedAllocations::Wait;
    if (found == MemoryPressure::OutOfMemory) {
        delayed = DelayedAllocations::Refuse;
    }
    auto const bounds = m_levels.bounds_of(found);
    if (!set_free_memory_signal(bounds.lower, bounds.upper, m_levels.delay_level(), &m_signal,
                                delayed)) {
        return ustl::NONE;
    }
    return ustl::some(deadline);
}

namespace {
    /// The init hook takes a plain function, as the thread does.
    INIT_CODE
    auto start_memory_watchdog() -> void {
        auto const levels = MemoryPressureLevels::from_boot_options();
        auto const status = MemoryWatchdog::get().start(levels);
        if (Status::InvArgs == status) {
            log::error("mm: the kernel.oom levels {}/{}/{} MiB with a debounce of {} MiB do not "
                       "nest; no memory watchdog",
                       levels.out_of_memory() / FRAMES_PER_MIB, levels.critical() / FRAMES_PER_MIB,
                       levels.warning() / FRAMES_PER_MIB, levels.debounce() / FRAMES_PER_MIB);
        } else if (Status::Ok != status) {
            log::error("mm: no thread for the memory watchdog: {}", to_string(status));
        }
    }
    // Once threads can be made: the DPC workers start on Threading itself.
    GKTL_INIT_HOOK(MemoryWatchdog, start_memory_watchdog, gktl::InitLevel::Threading + 1);
} // namespace

} // namespace ours::mm
