/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_COMMON_HPP
#define OURS_TASK_SCHED_COMMON_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/task/sched/types.hpp>

#include <array>

#include <ffl/wider.hpp>

namespace ours::task::sched {
/// How much heavier one priority is than the one below it, as an exact ratio.
///
/// 49/40 -- one priority level is worth 22.5% more CPU than the one below it,
/// and the thirty-one levels of the table therefore span a factor of about
/// 542. This is the one number that says what a priority *means*, which is
/// why it is the input and the table is the output: adding priority levels
/// must not silently change what one level is worth, and it would if the
/// table were parameterised by its span instead.
///
/// Written as a ratio rather than a decimal because the kernel has no
/// floating point (`-mno-sse`), and because an exact rational makes the
/// generated values reproducible rather than dependent on how a constant
/// folded.
constexpr i64 WEIGHT_RATIO_NUMERATOR = 49;
constexpr i64 WEIGHT_RATIO_DENOMINATOR = 40;

/// How many priority levels there are. Raising this regenerates the table;
/// see the assertions below for the point at which the format runs out.
constexpr usize NUM_PRIORITIES = 32;

/// Priority to weight.
///
/// Generated, not written out. A literal table is a set of numbers whose
/// relationship to each other is a claim in a comment: nothing checks that
/// entry 17 really is 1.225 times entry 16, and extending the table means
/// recomputing thirty-two values by hand and hoping. Here the relationship is
/// the code and the numbers are its output. (These are the same thirty-two
/// values the literal table held, to the unit -- it was itself 1.225^k, which
/// is how this rewrite could be checked.)
constexpr std::array<Weight, NUM_PRIORITIES> PRIORITY_TO_WEIGHT_MAP = [] {
    using Accumulator = ffl::Wider<i64>;

    // Guard digits carried below the stored format while stepping down the
    // table. Rounding to the stored sixteen bits at every step would compound,
    // and it would compound worst exactly where it hurts: at the bottom of the
    // table one unit in the last place is nearly a percent, so the ratio
    // between adjacent low priorities would stop being the ratio above them.
    // Forty-eight spare bits put the per-step error far below the final
    // rounding, and the widest intermediate is 70 bits -- comfortably inside
    // the 128-bit accumulator.
    constexpr usize GUARD_BITS = 48;
    constexpr Accumulator GUARD_HALF = Accumulator{1} << (GUARD_BITS - 1);

    std::array<Weight, NUM_PRIORITIES> map{};

    // Built downward from exactly 1.0, never upward from the smallest weight.
    // The top of the table is the value every other entry is a fraction of, so
    // it is the one that has to be exact; accumulating up to it would leave it
    // a rounding error short of 1.0 and quietly break the invariant asserted
    // below.
    auto accumulator = Accumulator{Weight::Format::ONE} << GUARD_BITS;
    map[NUM_PRIORITIES - 1] = Weight::from_raw(Weight::Format::ONE);

    for (usize i = NUM_PRIORITIES - 1; i > 0; --i) {
        accumulator = (accumulator * WEIGHT_RATIO_DENOMINATOR + WEIGHT_RATIO_NUMERATOR / 2) /
                      WEIGHT_RATIO_NUMERATOR;
        map[i - 1] = Weight::from_raw(static_cast<i64>((accumulator + GUARD_HALF) >> GUARD_BITS));
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
/// fraction of the heaviest thread" depends on it, and it is what the
/// downward construction above exists to guarantee.
static_assert(MAX_WEIGHT == Weight{1});

/// Adjacent entries really stand in the declared ratio. This is the property
/// a literal table could only assert in prose. The bound is the numerator:
/// `map[i - 1]` is `map[i] * DEN / NUM` carried at extra precision and then
/// rounded once, so the two cross-products can differ by at most one rounding
/// of the numerator -- and they do not drift with the table's size.
static_assert(
    [] {
        for (usize i = 1; i < NUM_PRIORITIES; ++i) {
            auto const lower = PRIORITY_TO_WEIGHT_MAP[i - 1].raw() * WEIGHT_RATIO_NUMERATOR;
            auto const upper = PRIORITY_TO_WEIGHT_MAP[i].raw() * WEIGHT_RATIO_DENOMINATOR;
            auto const deviation = lower > upper ? lower - upper : upper - lower;
            if (deviation > WEIGHT_RATIO_NUMERATOR) {
                return false;
            }
        }
        return true;
    }(),
    "the generated weights do not stand in WEIGHT_RATIO_NUMERATOR/DENOMINATOR");

/// A higher priority has to mean a strictly larger weight.
///
/// This is also the guard on extending the table, and it is the first one to
/// fire. Each step down divides by 1.225, so the entries shrink geometrically
/// while the format's resolution does not; past about fifty priorities at
/// sixteen fractional bits two adjacent entries round to the same number and
/// the bottom of the range stops distinguishing anything. The fix then is to
/// give Weight more fractional bits, not to shorten the table.
static_assert(
    [] {
        for (usize i = 1; i < NUM_PRIORITIES; ++i) {
            if (PRIORITY_TO_WEIGHT_MAP[i] <= PRIORITY_TO_WEIGHT_MAP[i - 1]) {
                return false;
            }
        }
        return true;
    }(),
    "the table has outgrown Weight's format; give it more fractional bits");

/// No weight may be zero. A zero weight is not a small share, it is a thread
/// that contributes nothing to the run queue's weight sum and then divides by
/// it.
static_assert(MIN_WEIGHT > Weight{0});

/// The smallest slice a thread is meant to be given in a single round.
///
/// A constant, not a per-CPU field. It never varied per CPU -- every RunQueue
/// initialised it from the same expression -- and a value that is the same on
/// every CPU costs a cache line in each run queue to say so.
constexpr auto MIN_SCHED_GRANULARITY = Milliseconds(1);

/// The targeted latency: the window in which every runnable thread should
/// get the CPU once.
///
/// Each thread then runs for a slice proportional to its weight over the total
/// weight of everything runnable. A smaller target is better interactivity and
/// a closer approximation of perfect multitasking, paid for in switching cost
/// and so in throughput.
constexpr auto TARGETED_LATENCY = Milliseconds(8);

/// How many granularity units make up one scheduling period.
constexpr usize SCHEDULING_PERIOD_GRANS = TARGETED_LATENCY / MIN_SCHED_GRANULARITY;

/// The unit the scheduling period is actually built from.
///
/// **This is TARGETED_LATENCY, not MIN_SCHED_GRANULARITY, and that is wrong.**
/// The field this replaces was named `minimal_granularity`, its comment said
/// "the smallest timeslice a thread is allocated in a single round", and it
/// was initialised from the 8 ms latency with the unused 1 ms constant sitting
/// directly above it -- so scheduling_period() answers 64 ms, and 64 ms is
/// every thread's slice base today.
///
/// It is preserved here deliberately. Correcting it multiplies the
/// context-switch rate by eight, which is a behaviour change and belongs in
/// its own commit with its own measurements, not smuggled in under a move of
/// some constants. See docs/repairs/260917/kernel-task-sched.md.
constexpr SchedDuration SCHED_GRANULARITY = to_sched_duration(TARGETED_LATENCY);

/// The period in which every runnable thread executes once.
///
/// Free function rather than a RunQueue member: it reads nothing per-CPU, and
/// leaving it on RunQueue meant a discipline had to be handed the whole run
/// queue to ask a question with a compile-time answer.
FORCE_INLINE
constexpr auto scheduling_period() -> SchedDuration {
    return SCHED_GRANULARITY * static_cast<i64>(SCHEDULING_PERIOD_GRANS);
}

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
