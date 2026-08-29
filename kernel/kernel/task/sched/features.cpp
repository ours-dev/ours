/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/task/sched/features.hpp>

namespace ours::task::sched {
/// Answers the runtime form of the same question the SCHED_* switches
/// answer at compile time.
///
/// Each feature is wired to the switch that already gates it, so the two
/// forms cannot disagree: Fair::try_to_delay_dequeue() asks this, and its
/// only caller is itself guarded by `if constexpr (SCHED_DELAY_DEQUEUE)`.
auto has_feature(Feature features) -> bool {
    auto enabled = Feature{};

    if constexpr (SCHED_DELAY_DEQUEUE) {
        enabled |= Feature::DelayDequeue;
    }

    // TODO(SmallHuaZi): Feature::HpTick has no switch of its own. It is
    // deliberately not tied to SCHED_NOHZ, which answers a different
    // question -- whether the tick stops while idle, not how precise it is.

    return (enabled & features) == features;
}
} // namespace ours::task::sched
