/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_FEATURES_HPP
#define OURS_TASK_SCHED_FEATURES_HPP 1

// USTL_ENABLE_ENUM_BITMASK below comes from here. The header used to rely on
// some earlier include dragging it in, so it only compiled when included after
// one -- including it first, as features.cpp does, failed.
#include <ustl/util/enum_bits.hpp>

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