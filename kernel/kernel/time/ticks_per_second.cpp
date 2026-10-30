/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/time/ticks_per_second.hpp>

#include <atomic>

namespace ours::time {
namespace {
    /// The rate of arch::Tick, in counts per second; 0 until it is set.
    ///
    /// An atomic that is only ever loaded and stored, with relaxed order, and
    /// a plain u64 would not do, because of who reads and who writes. The
    /// writer is one call, on the boot CPU, early in the boot. The readers
    /// are the console logger on every CPU, in any context: with interrupts
    /// off, in an NMI, in a panic. An NMI can land on the boot CPU in the
    /// middle of the writer's own store. To the language that is a data race
    /// on a plain u64 even though both sides run on one CPU, and undefined
    /// behaviour: the compiler may split the store, or keep an earlier load
    /// in a register across a loop that waits for the rate. A lock-free
    /// atomic is the one object the language lets an interrupt handler read
    /// while the code it interrupted writes it. A relaxed load and store
    /// compile to the same single mov a plain u64 would, on every
    /// architecture this runs on.
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
} // namespace

auto ticks_per_second() -> u64 {
    return TICKS_PER_SECOND.load(std::memory_order_relaxed);
}

auto set_ticks_per_second(u64 const rate) -> void {
    DEBUG_ASSERT(rate != 0, "the counter's rate cannot be 0");
    [[maybe_unused]]
    auto const previous = TICKS_PER_SECOND.exchange(rate, std::memory_order_relaxed);
    DEBUG_ASSERT(previous == 0, "the counter's rate is already {} Hz; it is set once", previous);
}
} // namespace ours::time
