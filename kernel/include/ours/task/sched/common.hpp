/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_COMMON_HPP
#define OURS_TASK_SCHED_COMMON_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/task/sched/types.hpp>

#include <array>
#include <iterator> // std::size

#include <ffl/mul_shr.hpp>

namespace ours::task::sched {
/// Priority to weight.
///
/// The table is geometric: each step up multiplies the weight by roughly
/// 1.225, so the *ratio* between two priorities depends only on the distance
/// between them, not on where they sit. That is the property the fair
/// discipline needs -- a thread one priority above another should get a fixed
/// share more CPU whether both are near the bottom or near the top -- and it
/// is why the table is written out rather than computed as `priority * k`.
/// Linux's sched_prio_to_weight[] is the same construction with a ratio of
/// 1.25 per nice level.
constexpr Weight PRIORITY_TO_WEIGHT_MAP[] = {
    121,   149,   182,   223,   273,   335,   410,   503,   616,   754,  924,
    1132,  1386,  1698,  2080,  2549,  3122,  3825,  4685,  5739,  7030, 8612,
    10550, 12924, 15832, 19394, 23757, 29103, 35651, 43672, 53499, 65536};

constexpr usize NUM_PRIORITIES = std::size(PRIORITY_TO_WEIGHT_MAP);

constexpr Weight MIN_WEIGHT = PRIORITY_TO_WEIGHT_MAP[0];

constexpr Weight MID_WEIGHT = PRIORITY_TO_WEIGHT_MAP[NUM_PRIORITIES / 2];

constexpr Weight MAX_WEIGHT = PRIORITY_TO_WEIGHT_MAP[NUM_PRIORITIES - 1];

constexpr usize LOWEST_PRIORITY = 0;

constexpr usize HIGHEST_PRIORITY = NUM_PRIORITIES - 1;

constexpr usize DEFAULT_PRIORITY = HIGHEST_PRIORITY >> 1;
constexpr usize IDLE_PRIORITY = DEFAULT_PRIORITY >> 1;

/// The scale of an inverse weight: an inverse weight is `2^WMULT_SHIFT / w`.
///
/// 32 is not arbitrary. It has to be large enough that the quantisation error
/// of the reciprocal is negligible against the weights in use -- with the
/// largest weight at 65536 the smallest reciprocal is 2^16, so the relative
/// error is under 2^-16 -- and small enough that every reciprocal still fits
/// in 32 bits, which is what keeps the scaling multiply a 64x32 one. Linux
/// picked the same number for the same two reasons (WMULT_SHIFT in
/// kernel/sched/fair.c).
constexpr usize WMULT_SHIFT = 32;

/// An inverse weight. Deliberately 32 bits wide rather than usize: it exists
/// only to be the right-hand operand of a widening multiply, and a type that
/// cannot hold a weight is a type that cannot be passed where a weight goes.
using Wmult = u32;

/// Priority to inverse weight, `2^WMULT_SHIFT / weight` rounded to nearest.
///
/// Dividing by a weight is how a share is turned into a time slice, and a
/// 64-bit division is not a thing to do on every scheduler tick -- on x86-64
/// a 128-bit one is worse still, since it becomes a call to __divti3, a
/// software loop in the compiler runtime. Pre-computing the reciprocal turns
/// each of those divisions into a multiply and a shift.
///
/// Generated from PRIORITY_TO_WEIGHT_MAP rather than written out. Linux ships
/// sched_prio_to_wmult[] as a second literal table beside sched_prio_to_weight[],
/// which means the two can disagree; here they cannot, because there is only
/// one table and the other is a function of it.
constexpr std::array<Wmult, NUM_PRIORITIES> PRIORITY_TO_WMULT_MAP = [] {
    std::array<Wmult, NUM_PRIORITIES> map{};
    for (usize i = 0; i < NUM_PRIORITIES; ++i) {
        map[i] = static_cast<Wmult>(ffl::reciprocal<u64>(PRIORITY_TO_WEIGHT_MAP[i], WMULT_SHIFT));
    }
    return map;
}();

constexpr Wmult MID_WMULT = PRIORITY_TO_WMULT_MAP[NUM_PRIORITIES / 2];

/// The table has to be strictly increasing for a higher priority to mean more
/// CPU, and everything below assumes it. Checking it here costs nothing and
/// catches an edit that transposes two entries -- which would otherwise show
/// up only as a scheduling anomaly nobody would trace back to a table.
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

/// The reciprocals must fit the narrow type they are stored in. They do for
/// every weight above 1; the assertion is here so that lowering MIN_WEIGHT
/// fails the build rather than silently truncating.
static_assert(
    [] {
        for (usize i = 0; i < NUM_PRIORITIES; ++i) {
            if (ffl::reciprocal<u64>(PRIORITY_TO_WEIGHT_MAP[i], WMULT_SHIFT) >
                std::numeric_limits<Wmult>::max()) {
                return false;
            }
        }
        return true;
    }(),
    "an inverse weight does not fit in Wmult; WMULT_SHIFT is too large for MIN_WEIGHT");

/// And the pair must actually round-trip: scaling a value by a weight and then
/// by that weight's reciprocal has to give the value back. This is the
/// property every user of the tables relies on, so it is worth stating as an
/// assertion rather than inferring it from how the reciprocal was computed.
static_assert(
    [] {
        for (usize i = 0; i < NUM_PRIORITIES; ++i) {
            auto const weight = PRIORITY_TO_WEIGHT_MAP[i];
            if (ffl::mul_shr_round<u64>(weight * 1000, PRIORITY_TO_WMULT_MAP[i], WMULT_SHIFT) !=
                1000) {
                return false;
            }
        }
        return true;
    }(),
    "a weight and its inverse do not round-trip");

/// HIGHEST_PRIORITY is the table's last index, so the range is closed at
/// both ends. The check used to be `priority < HIGHEST_PRIORITY`, which
/// rejected the one priority the name promises is valid; nothing tripped it
/// only because this function had no callers.
FORCE_INLINE
constexpr auto priority_to_weight(usize priority) -> Weight {
    DEBUG_ASSERT(LOWEST_PRIORITY <= priority && priority <= HIGHEST_PRIORITY);
    return PRIORITY_TO_WEIGHT_MAP[priority];
}

FORCE_INLINE
constexpr auto priority_to_wmult(usize priority) -> Wmult {
    DEBUG_ASSERT(LOWEST_PRIORITY <= priority && priority <= HIGHEST_PRIORITY);
    return PRIORITY_TO_WMULT_MAP[priority];
}

/// The reciprocal of an arbitrary weight.
///
/// The table covers the weights a single thread can have; a run queue's total
/// load is a sum of them and is not in the table, so it needs this. It does
/// divide -- there is no avoiding one somewhere -- which is why the answer is
/// meant to be cached alongside the weight it came from and recomputed only
/// when that weight changes, never on the path that uses it. Linux keeps the
/// pair together for exactly this reason: struct load_weight holds `weight`
/// and `inv_weight` side by side, and update_load_add() invalidates the
/// second whenever it touches the first.
FORCE_INLINE
constexpr auto weight_to_wmult(Weight weight) -> Wmult {
    // A zero-weight queue is an empty one. Returning the largest reciprocal
    // makes a subsequent scaling saturate rather than trap, which is the
    // failure mode that leaves the scheduler running; the callers that care
    // check for an empty queue before they get here.
    if (weight == Weight{0}) [[unlikely]] {
        return std::numeric_limits<Wmult>::max();
    }
    // A weight of 1 would give exactly 2^WMULT_SHIFT, one past what Wmult
    // holds. Clamping is right rather than merely convenient: the reciprocal
    // is already at its largest there, and one unit of it is below the
    // precision anything downstream can see.
    return ffl::clamping_cast<Wmult>(ffl::reciprocal<u64>(weight, WMULT_SHIFT));
}

/// `value * weight / divisor`, given the divisor's pre-computed reciprocal.
///
/// The product is formed at 128 bits before the shift narrows it, so a large
/// duration times a large weight cannot wrap -- the intermediate that would
/// overflow is never materialised in 64 bits. Linux, working without a
/// 128-bit type, has to shift the operands down in a loop first
/// (__calc_delta's `while (unlikely(w >= WMULT_CONST))`), losing low bits to
/// get the same safety.
///
/// Rounds to nearest rather than truncating. These results are accumulated
/// over a thread's lifetime, and truncation errs downward every time, so its
/// error grows with the number of ticks instead of cancelling out.
FORCE_INLINE
constexpr auto scale_by_wmult(u64 value, Weight weight, Wmult wmult) -> u64 {
    // The order of the two multiplies is not free. `weight * wmult` is at
    // most about 2^WMULT_SHIFT times the ratio the two describe, so it fits
    // 64 bits comfortably and leaves `value` a full 64 bits of room inside
    // the 128-bit product. Multiplying `value * weight` first would overflow
    // in 64 bits before the widening ever happened. Linux composes it the
    // same way round in __calc_delta().
    auto const factor = static_cast<u64>(weight) * static_cast<u64>(wmult);
    return ffl::mul_shr_round<u64>(value, factor, WMULT_SHIFT);
}
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_COMMON_HPP
