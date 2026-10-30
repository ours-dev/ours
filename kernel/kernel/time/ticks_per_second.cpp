/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/time/ticks_per_second.hpp>

#include <atomic>

namespace ours::time {
namespace {
    /// The rate of arch::Tick, in counts per second; 0 until it is set.
    ///
    /// An atomic that is loaded with a relaxed load and set once, with a
    /// relaxed compare-exchange. A plain u64 would not do, because of who
    /// reads and who writes. The writer is one call, on the boot CPU, early
    /// in the boot. The readers are the console logger on every CPU, in any
    /// context: with interrupts off, in an NMI, in a panic. An NMI can land
    /// on the boot CPU in the middle of the writer's own store. To the
    /// language that is a data race on a plain u64 even though both sides run
    /// on one CPU, and a data race is undefined behaviour: the compiler may
    /// assume nothing else writes the object while it reads it, so it may
    /// keep an earlier load in a register across a loop that waits for the
    /// rate. (Splitting a store in two is the same race on a target without a
    /// native 64-bit store; none this runs on is one.) A lock-free atomic is
    /// the one object the language lets an interrupt handler read while the
    /// code it interrupted writes it. A relaxed load compiles to the same
    /// single mov a plain u64 would, on every architecture this runs on.
    ///
    /// Relaxed is enough because the rate publishes nothing else. A reader
    /// that sees non-zero goes on to use the rate alone, and one that sees 0
    /// has an answer for that. The order the other CPUs rely on -- the rate
    /// is set before they start -- is made by the way the boot CPU starts
    /// them, not by this store.
    ///
    /// constinit, so that it is in place before any static constructor runs:
    /// the logger reads it from the first line of the boot.
    constinit std::atomic<u64> TICKS_PER_SECOND{0};

    // An atomic that is not lock-free is emulated with a lock the compiler's
    // runtime library keeps, and a reader in an NMI that landed inside the
    // writer would wait on a lock its own CPU holds. This says there is none.
    static_assert(std::atomic<u64>::is_always_lock_free,
                  "NMI and panic readers cannot take a lock");
} // namespace

auto ticks_per_second() -> u64 {
    return TICKS_PER_SECOND.load(std::memory_order_relaxed);
}

auto set_ticks_per_second(u64 const rate) -> void {
    DEBUG_ASSERT(rate != 0, "the counter's rate cannot be 0");
    // The first call's rate is the one that stays, in every build: a second
    // call finds the variable non-zero and changes nothing, so what the
    // readers may rely on holds even where the assertion below is compiled
    // out. Compare-exchange rather than a load and a store, so that a second
    // call made at the same moment by another CPU is caught as well. The
    // strong form, because a weak one may fail with the variable still 0 on
    // the architectures where it is a load-reserved/store-conditional pair,
    // and a first call that failed would leave the rate unset in a release
    // build and trip the assertion in a debug one.
    auto expected = u64{0};
    [[maybe_unused]]
    auto const first = TICKS_PER_SECOND.compare_exchange_strong(
        expected, rate, std::memory_order_relaxed, std::memory_order_relaxed);
    DEBUG_ASSERT(first, "the counter's rate is already {} Hz; it is set once", expected);
}
} // namespace ours::time
