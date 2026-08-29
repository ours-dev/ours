/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_TYPES_HPP
#define OURS_TASK_SCHED_TYPES_HPP 1

#include <ours/macro_abi.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/types.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::task::sched {
using SchedWeight = i32;
using Weight = usize;
using SchedDuration = Duration;
using SchedTime = TimePoint<>;

enum class EnqueueControl;
enum class DequeueControl;

// [[=bits::bitmask]]
enum class UnparkControl {
    None,
    Initial = (1 << 0),
    ThisCpu = (1 << 1),
};
USTL_ENABLE_ENUM_BITMASK(UnparkControl);

struct Runtime;
class Scheduler;
class ISchedule;
class Fair;

struct BaseProfile;

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_TYPES_HPP
