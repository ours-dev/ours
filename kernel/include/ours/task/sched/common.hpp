/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_COMMON_HPP
#define OURS_TASK_SCHED_COMMON_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/task/sched/types.hpp>

#include <array>
#include <iterator> // std::size

namespace ours::task::sched {
namespace detail {
    /// The numerators of the priority-to-weight table, over Weight::Format::ONE.
    ///
    /// The table is geometric: each step up multiplies the weight by roughly
    /// 1.225, so the *ratio* between two priorities depends only on the distance
    /// between them, not on where they sit. That is the property the fair
    /// discipline needs -- one priority of separation should be worth a fixed
    /// share whether both threads are near the bottom of the range or near the
    /// top -- and it is why the table is written out rather than computed as
    /// `priority * k`. The last entry is the scale itself, so the highest
    /// priority weighs exactly 1.0 and every other entry reads as a fraction of
    /// it.
    ///
    /// Zircon's kPriorityToWeightTable is the same construction at a ratio of 1.1
    /// per priority with ten fractional bits, ending at 1024.
    constexpr i64 WEIGHT_NUMERATORS[] = {121,   149,   182,   223,   273,   335,   410,   503,
                                         616,   754,   924,   1132,  1386,  1698,  2080,  2549,
                                         3122,  3825,  4685,  5739,  7030,  8612,  10550, 12924,
                                         15832, 19394, 23757, 29103, 35651, 43672, 53499, 65536};
} // namespace detail

constexpr usize NUM_PRIORITIES = std::size(detail::WEIGHT_NUMERATORS);

/// Priority to weight.
///
/// Built from the numerators rather than written out as `Weight` literals,
/// which keeps the table above readable as plain integers -- the same reason
/// Zircon wraps its entries in WeightTableEntry, whose converting constructor
/// is `ffl::FromRatio<int64_t>(value, SchedWeight::Format::Power)`.
constexpr std::array<Weight, NUM_PRIORITIES> PRIORITY_TO_WEIGHT_MAP = [] {
    std::array<Weight, NUM_PRIORITIES> map{};
    for (usize i = 0; i < NUM_PRIORITIES; ++i) {
        map[i] = Weight::from_ratio(detail::WEIGHT_NUMERATORS[i], Weight::Format::ONE);
    }
    return map;
}();

constexpr Weight MIN_WEIGHT = PRIORITY_TO_WEIGHT_MAP[0];

constexpr Weight MID_WEIGHT = PRIORITY_TO_WEIGHT_MAP[NUM_PRIORITIES / 2];

constexpr Weight MAX_WEIGHT = PRIORITY_TO_WEIGHT_MAP[NUM_PRIORITIES - 1];

constexpr usize LOWEST_PRIORITY = 0;

constexpr usize HIGHEST_PRIORITY = NUM_PRIORITIES - 1;

constexpr usize DEFAULT_PRIORITY = HIGHEST_PRIORITY >> 1;
constexpr usize IDLE_PRIORITY = DEFAULT_PRIORITY >> 1;

/// The top of the range is exactly 1.0. Everything that reads a weight as "a
/// fraction of the heaviest thread" depends on it, and an edit that appended
/// an entry or rescaled the table would otherwise break that silently.
static_assert(MAX_WEIGHT == Weight{1});

/// A higher priority has to mean a larger weight, and every ratio computed
/// from the table assumes it. A transposed pair would otherwise surface only
/// as a scheduling anomaly nobody would trace back to a table.
static_assert(
    [] {
        for (usize i = 1; i < NUM_PRIORITIES; ++i) {
            if (PRIORITY_TO_WEIGHT_MAP[i] <= PRIORITY_TO_WEIGHT_MAP[i - 1]) {
                return false;
            }
        }
        return true;
    }(),
    "PRIORITY_TO_WEIGHT_MAP must be strictly increasing");

/// No weight may be zero. A zero weight is not a small share, it is a thread
/// that contributes nothing to the run queue's weight sum and divides by it.
static_assert(MIN_WEIGHT > Weight{0});

/// HIGHEST_PRIORITY is the table's last index, so the range is closed at
/// both ends. The check used to be `priority < HIGHEST_PRIORITY`, which
/// rejected the one priority the name promises is valid; nothing tripped it
/// only because this function had no callers.
FORCE_INLINE
constexpr auto priority_to_weight(usize priority) -> Weight {
    DEBUG_ASSERT(LOWEST_PRIORITY <= priority && priority <= HIGHEST_PRIORITY);
    return PRIORITY_TO_WEIGHT_MAP[priority];
}

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_COMMON_HPP
