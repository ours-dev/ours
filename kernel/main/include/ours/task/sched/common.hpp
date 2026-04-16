/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_COMMON_HPP
#define OURS_TASK_SCHED_COMMON_HPP 1

#include <ours/task/sched/types.hpp>

#include <ours/compiler.hpp>

#include <iterator> // std::size

namespace ours::task::sched {
    static constexpr Weight PRIORITY_TO_WEIGHT_MAP[] = {
        121,   149,   182,   223,   273,   335,   410,   503,   616,   754,  924,
        1132,  1386,  1698,  2080,  2549,  3122,  3825,  4685,  5739,  7030, 8612,
        10550, 12924, 15832, 19394, 23757, 29103, 35651, 43672, 53499, 65536
    };

    static constexpr Weight MIN_WEIGHT = PRIORITY_TO_WEIGHT_MAP[0];

    static constexpr Weight MID_WEIGHT = PRIORITY_TO_WEIGHT_MAP[std::size(PRIORITY_TO_WEIGHT_MAP) / 2];

    static constexpr usize LOWEST_PRIORITY = 0;

    static constexpr auto HIGHEST_PRIORITY = std::size(PRIORITY_TO_WEIGHT_MAP) - 1;

    static constexpr usize DEFAULT_PRIORITY = HIGHEST_PRIORITY >> 1;
    static constexpr usize IDLE_PRIORITY = DEFAULT_PRIORITY >> 1;

    FORCE_INLINE
    constexpr auto priority_to_weight(usize priority) -> Weight {
        DEBUG_ASSERT(LOWEST_PRIORITY <= priority && priority < HIGHEST_PRIORITY);
        return PRIORITY_TO_WEIGHT_MAP[priority];
    }
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_COMMON_HPP