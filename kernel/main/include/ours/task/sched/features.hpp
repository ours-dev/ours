/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_FEATURES_HPP
#define OURS_TASK_SCHED_FEATURES_HPP 1

#include <ours/macro_abi.hpp>

namespace ours::task::sched {
    enum class Feature {
        DelayDequeue = BIT(0),

        HpTick = BIT(1),
    };

    auto has_feature(Feature features) -> bool;

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_FEATURES_HPP