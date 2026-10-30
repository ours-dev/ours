/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/boot/options.hpp>
#include <ours/init.hpp>
#include <ours/mm/memory_watchdog.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <atomic>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::mm {
namespace {
    constexpr usize FRAMES_PER_MIB = 1_mb / PAGE_SIZE;
    constexpr usize NO_BOUND = ~usize{0};

    /// What wakes the watchdog: the PMM's free-memory signal, set_levels(),
    /// and a Hold that goes. Auto-unsignal, so each wakes it once.
    sync::Event SIGNAL{sync::Event::Options::AutoUnsignal};

    /// Held while the watchdog works out the pressure and arms the PMM, and
    /// for as long as a Hold lives; guards LEVELS and SKIP_HYSTERESIS. Not
    /// held while it sleeps.
    sync::Mutex LOCK;
    MemoryPressureLevels LEVELS{};

    /// set_levels() asks for the next look to declare what it finds, lower
    /// pressure or not: a test moves the levels and wants the answer now.
    bool SKIP_HYSTERESIS = false;

    /// What pressure() answers: written by the watchdog alone.
    std::atomic<MemoryPressure> PRESSURE{MemoryPressure::Normal};

    auto pressure_for(MemoryPressureLevels const &levels, usize const free) -> MemoryPressure {
        if (free < levels.out_of_memory) {
            return MemoryPressure::OutOfMemory;
        }
        if (free < levels.critical) {
            return MemoryPressure::Critical;
        }
        if (free < levels.warning) {
            return MemoryPressure::Warning;
        }
        return MemoryPressure::Normal;
    }

    struct Bounds {
        usize lower;
        usize upper;
    };

    /// The free memory the PMM keeps quiet about at |pressure|: the range of
    /// that pressure, widened by the debounce on both sides, so that free
    /// memory moving to and fro across a level does not wake the watchdog
    /// each time it crosses.
    auto bounds_for(MemoryPressureLevels const &levels, MemoryPressure const pressure) -> Bounds {
        auto const d = levels.debounce;
        switch (pressure) {
            case MemoryPressure::Normal:
                return {.lower = levels.warning - d, .upper = NO_BOUND};
            case MemoryPressure::Warning:
                return {.lower = levels.critical - d, .upper = levels.warning + d};
            case MemoryPressure::Critical:
                return {.lower = levels.out_of_memory - d, .upper = levels.critical + d};
            case MemoryPressure::OutOfMemory:
                return {.lower = 0, .upper = levels.out_of_memory + d};
        }
        return {.lower = 0, .upper = NO_BOUND};
    }

    /// Looks at free memory, declares the pressure, arms the PMM for the next
    /// change, and sleeps until it comes.
    ///
    /// A higher pressure is declared at once; a lower one only once the
    /// hysteresis has passed since the last change, so that a load that
    /// comes and goes is not logged as a change each time. What the PMM is
    /// armed with follows the free memory found, not the pressure declared:
    /// the gate opens as soon as memory is back, the log waits.
    ///
    /// The delay level sits the debounce below the OutOfMemory level, which
    /// is the lower bound at Critical: free memory that falls past it shuts
    /// the gate and wakes this in the same allocation, and this then declares
    /// OutOfMemory and refuses the waiters, since nothing can be reclaimed.
    auto watchdog_routine(void *) -> i32 {
        auto declared = MemoryPressure::Normal;
        auto declared_at = time::SteadyClock::now();
        auto first = true;
        while (true) {
            auto deadline = time::Deadline::infinite();
            {
                Guard guard{LOCK};
                auto const levels = LEVELS;
                auto const free = approx_free_frame_count();
                auto const found = pressure_for(levels, free);
                auto const now = time::SteadyClock::now();

                auto declare = first || SKIP_HYSTERESIS || found > declared;
                if (!declare && found < declared) {
                    if (now - declared_at >= levels.hysteresis) {
                        declare = true;
                    } else {
                        deadline = time::Deadline::at(declared_at + levels.hysteresis);
                    }
                }
                SKIP_HYSTERESIS = false;
                if (declare && (first || found != declared)) {
                    log::info("mm: memory pressure {}, {} MiB free", to_string(found),
                              free / FRAMES_PER_MIB);
                    if (found == MemoryPressure::OutOfMemory) {
                        log::warn("mm: out of memory, and nothing to reclaim: allocations that "
                                  "could wait are refused");
                    }
                    declared = found;
                    declared_at = now;
                    PRESSURE.store(found, std::memory_order_release);
                }
                first = false;

                auto delayed = DelayedAllocations::Wait;
                if (found == MemoryPressure::OutOfMemory) {
                    delayed = DelayedAllocations::Refuse;
                }
                auto const bounds = bounds_for(levels, found);
                if (!set_free_memory_signal(bounds.lower, bounds.upper,
                                            levels.out_of_memory - levels.debounce, &SIGNAL,
                                            delayed)) {
                    // Free memory moved past the bounds while this looked.
                    continue;
                }
            }
            auto _ = SIGNAL.wait(deadline);
        }
    }

    INIT_CODE
    auto start_memory_watchdog() -> void {
        auto const levels = MemoryWatchdog::boot_levels();
        if (!levels.is_valid()) {
            log::error("mm: the kernel.oom levels {}/{}/{} MiB with a debounce of {} MiB do not "
                       "nest; no memory watchdog",
                       levels.out_of_memory / FRAMES_PER_MIB, levels.critical / FRAMES_PER_MIB,
                       levels.warning / FRAMES_PER_MIB, levels.debounce / FRAMES_PER_MIB);
            return;
        }
        {
            Guard guard{LOCK};
            LEVELS = levels;
        }

        auto *const thread = task::Thread::spawn("memory-watchdog", watchdog_routine, nullptr,
                                                 task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            log::error("mm: no thread for the memory watchdog");
            return;
        }
        auto _ = thread->detach();
        auto _ = thread->resume();
    }
    // Once threads can be made: the DPC workers start on Threading itself.
    GKTL_INIT_HOOK(MemoryWatchdog, start_memory_watchdog, gktl::InitLevel::Threading + 1);
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

auto MemoryPressureLevels::is_valid() const -> bool {
    return out_of_memory > debounce && critical > out_of_memory + debounce &&
           warning > critical + debounce;
}

auto MemoryWatchdog::pressure() -> MemoryPressure {
    return PRESSURE.load(std::memory_order_acquire);
}

auto MemoryWatchdog::levels() -> MemoryPressureLevels {
    Guard guard{LOCK};
    return LEVELS;
}

auto MemoryWatchdog::boot_levels() -> MemoryPressureLevels {
    auto const &options = boot::options();
    return {
        .out_of_memory = usize{options.oom_out_of_memory_mb()} * FRAMES_PER_MIB,
        .critical = usize{options.oom_critical_mb()} * FRAMES_PER_MIB,
        .warning = usize{options.oom_warning_mb()} * FRAMES_PER_MIB,
        .debounce = usize{options.oom_debounce_mb()} * FRAMES_PER_MIB,
        .hysteresis = duration_cast<Duration>(Seconds(options.oom_hysteresis_seconds())),
    };
}

auto MemoryWatchdog::set_levels(MemoryPressureLevels const &levels) -> Status {
    if (!levels.is_valid()) {
        return Status::InvArgs;
    }
    {
        Guard guard{LOCK};
        LEVELS = levels;
        SKIP_HYSTERESIS = true;
    }
    SIGNAL.signal();
    return Status::Ok;
}

// The analysis cannot follow a lock taken in one function and let go of in
// another: the Hold is the scope.
TA_NO_THREAD_SAFETY_ANALYSIS
MemoryWatchdog::Hold::Hold() {
    LOCK.acquire();
}

TA_NO_THREAD_SAFETY_ANALYSIS
MemoryWatchdog::Hold::~Hold() {
    LOCK.release();
    SIGNAL.signal();
}

} // namespace ours::mm
