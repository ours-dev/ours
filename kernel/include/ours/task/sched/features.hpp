/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_FEATURES_HPP
#define OURS_TASK_SCHED_FEATURES_HPP 1

#include <ours/macro_abi.hpp>

namespace ours::task::sched {
    // [[=bits::bitmask]]
    enum class Feature {
        DelayDequeue = 0_bit,

        HpTick = 1_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(Feature);

    auto has_feature(Feature features) -> bool;

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_FEATURES_HPP