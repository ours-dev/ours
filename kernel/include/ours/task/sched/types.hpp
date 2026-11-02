/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_TYPES_HPP
#define OURS_TASK_SCHED_TYPES_HPP 1

#include <ours/chrono.hpp>
#include <ours/compiler.hpp>
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
/// machine".
///
/// Signed, although a weight is never negative: the types it is used with
/// (SchedDuration, SchedTime) are signed, and a single unsigned operand
/// promotes a whole expression to unsigned, where a subtraction that should
/// have gone slightly negative instead becomes enormous. Keeping the sign is
/// cheaper than auditing every expression.
///
/// Sixteen fractional bits, one for each bit of the existing weight table's
/// scale: its largest entry is 65536, so the table's numbers are already
/// raw values of this format and nothing has to be renumbered.
using Weight = ffl::Fixed<i64, 16>;
/// The scheduler's time and duration, in nanoseconds.
///
/// Fixed-point with *zero* fractional bits, which sounds like a long way to
/// spell i64 and is not. The point is that they share one arithmetic with
/// Weight: `duration * weight` and `vruntime / weight_sum` are then ordinary
/// operators on ordinary values, with the widening, the rounding and the
/// saturation already decided in one place.
///
/// The cost is that a time and a duration are now the same type, so
/// `time + time` compiles where the std::chrono pair these replace would have
/// rejected it. That is a real loss, taken deliberately: the alternative is a
/// phantom tag parameter on Fixed that ffl does not have, and the scheduler's
/// expressions are short enough to read.
using SchedDuration = ffl::Fixed<i64, 0>;
using SchedTime = ffl::Fixed<i64, 0>;

/// The boundary between the kernel clock and the scheduler's arithmetic.
///
/// The kernel clock is typed (`TimePoint<SteadyClock>`), and that typing is
/// worth keeping outside the scheduler even though it cannot survive inside it.
/// Crossing it is therefore explicit, and these three functions are the only
/// places the conversion is spelled.
FORCE_INLINE
constexpr auto to_sched_time(TimePoint<> when) -> SchedTime {
    return SchedTime(when.time_since_epoch().count());
}

/// Templated over the period so that the readable constants keep their units:
/// `Milliseconds(8)` says what it is in a way `8'000'000` does not, and the
/// cast to nanoseconds belongs here rather than at each of those sites.
template <typename Rep, typename Period>
FORCE_INLINE
constexpr auto to_sched_duration(std::chrono::duration<Rep, Period> duration) -> SchedDuration {
    return SchedDuration(duration_cast<Duration>(duration).count());
}

FORCE_INLINE
constexpr auto to_time_point(SchedTime when) -> TimePoint<> {
    return TimePoint<>(Duration(when.raw()));
}

enum class EnqueueOptions;
enum class DequeueOptions;

// [[=bits::bitmask]]
enum class UnparkOptions {
    None,
    Initial = (1 << 0),
    ThisCpu = (1 << 1),
};
USTL_ENABLE_ENUM_BITMASK(UnparkOptions);

class RunQueue;
class Scheduler;
class ISchedule;
class Fair;
class RoundRobin;

struct BaseProfile;

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_TYPES_HPP
