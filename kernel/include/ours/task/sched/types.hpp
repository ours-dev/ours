/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_TYPES_HPP
#define OURS_TASK_SCHED_TYPES_HPP 1

#include <ours/macro_abi.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/types.hpp>

#include <ffl/fixed.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::task::sched {
/// A task's weight: a fixed-point fraction in (0.0, 1.0].
///
/// A weight is a *share*, not a count. Only ratios between weights ever mean
/// anything -- a thread gets `weight / total_weight` of the CPU -- so the
/// scale is free, and normalising the top of the range to exactly 1.0 is what
/// makes every value readable as "this fraction of the best thread on the
/// machine". Zircon defines SchedWeight the same way, as ffl::Fixed<int64_t, 10>.
///
/// Signed, although a weight is never negative, for the reason Zircon gives:
/// the types it is used with (SchedDuration, SchedTime) are signed, and a
/// single unsigned operand promotes a whole expression to unsigned, where a
/// subtraction that should have gone slightly negative instead becomes
/// enormous. Keeping the sign is cheaper than auditing every expression.
///
/// Sixteen fractional bits, one for each bit of the existing weight table's
/// scale: its largest entry is 65536, so the table's numbers are already
/// raw values of this format and nothing has to be renumbered.
using Weight = ffl::Fixed<i64, 16>;
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
class RoundRobin;

struct BaseProfile;

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_TYPES_HPP
