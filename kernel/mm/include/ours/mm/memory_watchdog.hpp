/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_WATCHDOG_HPP
#define OURS_MM_MEMORY_WATCHDOG_HPP 1

#include <ours/status.hpp>
#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

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

/// Where each pressure begins, in frames of free memory: below |warning|
/// it is Warning, below |critical| Critical, below |out_of_memory|
/// OutOfMemory. The watchdog looks again only once free memory has gone
/// |debounce| frames past a level, and declares a lower pressure only
/// |hysteresis| after its last change.
struct MemoryPressureLevels {
    usize out_of_memory;
    usize critical;
    usize warning;
    usize debounce;
    Duration hysteresis;

    /// Each level above the one before by more than the debounce, and the
    /// lowest above it too: then every pressure has a range of its own that
    /// the debounce does not swallow.
    [[nodiscard]]
    auto is_valid() const -> bool;
};

/// A kernel thread that watches how much memory is free, through the PMM's
/// free-memory signal (set_free_memory_signal()), and turns it into a
/// pressure. It sets the PMM's gate for the allocations that can wait: they
/// wait below the OutOfMemory level, until the watchdog looks; and once it has
/// declared OutOfMemory, with nothing to reclaim, they are refused. Each change
/// of pressure is logged.
///
/// Started at boot with the levels the `kernel.oom.*` boot options give.
class MemoryWatchdog {
  public:
    /// The pressure last declared.
    [[nodiscard]]
    static auto pressure() -> MemoryPressure;

    /// The levels in use.
    [[nodiscard]]
    static auto levels() -> MemoryPressureLevels;

    /// The levels the boot options give.
    [[nodiscard]]
    static auto boot_levels() -> MemoryPressureLevels;

    /// Replaces the levels, and the watchdog looks again at once, without
    /// waiting for the hysteresis to declare a lower pressure. InvArgs for
    /// levels that are not is_valid(). For tests, which move the levels to
    /// where free memory is, rather than free memory to where they are.
    static auto set_levels(MemoryPressureLevels const &levels) -> Status;

    /// While one is alive, the watchdog arms nothing: for a test that drives
    /// the PMM's gate and signal by hand. Once it goes, the watchdog looks
    /// again and arms them for itself.
    class Hold {
      public:
        Hold();
        ~Hold();

        Hold(Hold const &) = delete;
        auto operator=(Hold const &) -> Hold & = delete;
    };
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_WATCHDOG_HPP
