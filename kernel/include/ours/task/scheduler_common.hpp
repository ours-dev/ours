/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHEDULER_COMMON_HPP
#define OURS_TASK_SCHEDULER_COMMON_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/task/scheduler_types.hpp>

#include <array>

#include <ffl/wider.hpp>

namespace ours::task {
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

/// The virtual time a thread of |weight| is charged for |real| of CPU time.
///
/// The Fair discipline hands out equal virtual time, and virtual time is real
/// time divided by weight: a heavier thread's clock runs slower, so equal
/// clocks mean it was given more real time. The scale is anchored at
/// MID_WEIGHT, the weight whose clock reads real time. DEFAULT_PRIORITY is one
/// step below the priority MID_WEIGHT belongs to, so a default thread's clock
/// runs 1.225 times as fast as the wall clock.
///
/// Divides where `weighted_offset()` multiplies. They are not interchangeable
/// and the two used to be one function, `calc_weighted_time()`: a heavier
/// thread then earned virtual time faster, and so was given less CPU, not more.
[[nodiscard]]
FORCE_INLINE
constexpr auto to_virtual_time(SchedDuration const real, Weight const weight) -> SchedDuration {
    DEBUG_ASSERT(weight > Weight{0});
    if (weight == MID_WEIGHT) {
        return real;
    }

    return real / (weight / MID_WEIGHT);
}

/// A thread's term in the run queue's weighted sum: |offset|, how far its
/// virtual time stands from the queue's zero, times its weight.
///
/// The sum of these over the queue divided by the sum of the weights is the
/// weighted mean offset, which is what makes a thread eligible or not. The unit
/// is weight times time, so it is never converted back with `to_virtual_time()`.
/// The product is exact: a whole number of nanoseconds times a weight is a whole
/// number of `WeightedTime` units, so a term can be taken out of the sum again,
/// or the whole sum moved to another zero, without leaving a remainder.
[[nodiscard]]
FORCE_INLINE
constexpr auto weighted_offset(SchedDuration const offset, Weight const weight) -> WeightedTime {
    return WeightedTime(offset) * weight;
}

/// The weighted mean of the offsets whose weighted sum is |sum| and whose
/// weights add up to |weight_sum|, rounded down to a nanosecond.
///
/// Down, so that a thread placed at the mean passes the eligibility test: the
/// test compares against the exact mean, and a mean rounded up could sit up to
/// half a nanosecond ahead of it.
[[nodiscard]]
FORCE_INLINE
constexpr auto weighted_mean_offset(WeightedTime const sum, Weight const weight_sum)
    -> SchedDuration {
    DEBUG_ASSERT(weight_sum > Weight{0});
    return SchedDuration((sum / weight_sum).floor());
}

/// Whether a thread at |offset| is owed service: it stands at or behind the
/// weighted mean, `sum / weight_sum`, compared without the division.
///
/// |sum| and |weight_sum| have to be taken over the same threads. A sum that
/// leaves the running thread out against a weight sum that counts it is not a
/// mean of anything.
[[nodiscard]]
FORCE_INLINE
constexpr auto offset_is_eligible(WeightedTime const sum, Weight const weight_sum,
                                  SchedDuration const offset) -> bool {
    return sum >= weighted_offset(offset, weight_sum);
}

/// MID_WEIGHT's clock reads real time.
static_assert(to_virtual_time(SchedDuration{1'000'000}, MID_WEIGHT) == SchedDuration{1'000'000});

/// A clock runs inversely to its weight, at every priority and for a
/// millisecond and for a thousand seconds: virtual time times weight is the same
/// for the same real time, up to the conversion's error. That is the rate kept
/// in sixteen fractional bits, 1/2539 of itself at worst, and the quotient's
/// truncation; the bound is 1/2048 of the expected product and a nanosecond.
/// Multiplying instead of dividing fails it by the square of
/// `weight / MID_WEIGHT`.
static_assert(
    [] {
        constexpr SchedDuration REALS[] = {
            SchedDuration{1'000'000},
            SchedDuration{1'000'000'000'000},
        };
        for (auto const real: REALS) {
            auto const expected = weighted_offset(real, MID_WEIGHT);
            for (usize i = 0; i < NUM_PRIORITIES; ++i) {
                auto const weight = PRIORITY_TO_WEIGHT_MAP[i];
                auto const virtual_time = to_virtual_time(real, weight);
                auto const deviation = weighted_offset(virtual_time, weight) - expected;
                if (deviation.absolute() > expected / i64{2048} + WeightedTime(SchedDuration{1})) {
                    return false;
                }
            }
        }
        return true;
    }(),
    "a thread's virtual time is not inversely proportional to its weight");

/// The heaviest thread's clock runs about 21 times slower than MID_WEIGHT's
/// and the lightest's about 26 times faster, which is the whole span of the
/// table seen from its middle. A default thread's runs 1.225 times as fast.
static_assert(to_virtual_time(SchedDuration{1'000'000}, MAX_WEIGHT) < SchedDuration{50'000});
static_assert(to_virtual_time(SchedDuration{1'000'000}, MAX_WEIGHT) > SchedDuration{45'000});
static_assert(to_virtual_time(SchedDuration{1'000'000}, MIN_WEIGHT) > SchedDuration{25'000'000});
static_assert(to_virtual_time(SchedDuration{1'000'000}, MIN_WEIGHT) < SchedDuration{26'000'000});
static_assert(to_virtual_time(SchedDuration{1'000'000}, priority_to_weight(DEFAULT_PRIORITY)) >
              SchedDuration{1'200'000});
static_assert(to_virtual_time(SchedDuration{1'000'000}, priority_to_weight(DEFAULT_PRIORITY)) <
              SchedDuration{1'250'000});

/// The same over eleven days, which a conversion that narrows its intermediate
/// to 64 bits gets wrong.
static_assert(to_virtual_time(SchedDuration{1'000'000'000'000'000}, MAX_WEIGHT) <
              SchedDuration{50'000'000'000'000});
static_assert(to_virtual_time(SchedDuration{1'000'000'000'000'000}, MAX_WEIGHT) >
              SchedDuration{45'000'000'000'000});
static_assert(to_virtual_time(SchedDuration{1'000'000'000'000'000}, MIN_WEIGHT) >
              SchedDuration{25'000'000'000'000'000});
static_assert(to_virtual_time(SchedDuration{1'000'000'000'000'000}, MIN_WEIGHT) <
              SchedDuration{26'000'000'000'000'000});

/// A term is exact: half of 3 is 1.5, not 1 or 2.
static_assert(weighted_offset(SchedDuration{3}, Weight::from_ratio(1, 2)) ==
              WeightedTime::from_ratio(3, 2));

/// A lone thread stands exactly at the mean, at every priority: its lag is
/// zero, not a rounding left over from dividing by a weight as small as 0.00185,
/// and a thread at the mean is eligible, the test being `>=`.
static_assert(
    [] {
        for (usize i = 0; i < NUM_PRIORITIES; ++i) {
            auto const weight = PRIORITY_TO_WEIGHT_MAP[i];
            for (auto const offset:
                 {SchedDuration{1}, SchedDuration{123'456'789}, SchedDuration{-98'765'432}}) {
                auto const sum = weighted_offset(offset, weight);
                if (weighted_mean_offset(sum, weight) != offset ||
                    !offset_is_eligible(sum, weight, offset)) {
                    return false;
                }
            }
        }
        return true;
    }(),
    "a lone thread does not stand at the weighted mean");

/// The mean is rounded down: the mean of 0 and 1 is 0, so a thread placed at the
/// mean is eligible, and so is one placed at the mean of -1 and 0, which is -1.
static_assert(
    [] {
        constexpr auto SUM = weighted_offset(SchedDuration{1}, MAX_WEIGHT);
        constexpr auto WEIGHT_SUM = MAX_WEIGHT + MAX_WEIGHT;
        constexpr auto NEGATIVE_SUM = weighted_offset(SchedDuration{-1}, MAX_WEIGHT);
        return weighted_mean_offset(SUM, WEIGHT_SUM) == SchedDuration{0} &&
               offset_is_eligible(SUM, WEIGHT_SUM, SchedDuration{0}) &&
               weighted_mean_offset(NEGATIVE_SUM, WEIGHT_SUM) == SchedDuration{-1} &&
               offset_is_eligible(NEGATIVE_SUM, WEIGHT_SUM, SchedDuration{-1});
    }(),
    "the weighted mean is not rounded down");

/// The weighted sum against the weight sum: two MID_WEIGHT threads whose
/// offsets are 100 ms and 0 average to 50 ms, and only the one ahead of the
/// average fails the eligibility test. The earlier helper, `calc_weighted_time()`,
/// divided the term by MID_WEIGHT as well and answered about 1050 ms, which made
/// both eligible.
static_assert(
    [] {
        constexpr auto AHEAD = SchedDuration{100'000'000};
        constexpr auto BEHIND = SchedDuration{0};
        constexpr auto SUM =
            weighted_offset(AHEAD, MID_WEIGHT) + weighted_offset(BEHIND, MID_WEIGHT);
        constexpr auto WEIGHT_SUM = MID_WEIGHT + MID_WEIGHT;

        return weighted_mean_offset(SUM, WEIGHT_SUM) == SchedDuration{50'000'000} &&
               offset_is_eligible(SUM, WEIGHT_SUM, BEHIND) &&
               !offset_is_eligible(SUM, WEIGHT_SUM, AHEAD);
    }(),
    "the weighted sum does not average the offsets of its threads");

/// The running thread is part of the mean, in the sum and in the weights alike.
/// A priority-31 thread running at 10 ms and a default thread waiting at 5 ms:
/// the mean is about 9.8 ms, so the waiting one is eligible and the running one,
/// just ahead, is not. Taking the sum over the waiting thread only while the
/// weights count both answers "ineligible" to the waiting thread.
static_assert(
    [] {
        constexpr auto RUNNING = SchedDuration{10'000'000};
        constexpr auto WAITING = SchedDuration{5'000'000};
        constexpr auto HEAVY = MAX_WEIGHT;
        constexpr auto LIGHT = priority_to_weight(DEFAULT_PRIORITY);
        constexpr auto SUM = weighted_offset(RUNNING, HEAVY) + weighted_offset(WAITING, LIGHT);
        constexpr auto WEIGHT_SUM = HEAVY + LIGHT;

        return offset_is_eligible(SUM, WEIGHT_SUM, WAITING) &&
               !offset_is_eligible(SUM, WEIGHT_SUM, RUNNING) &&
               !offset_is_eligible(weighted_offset(WAITING, LIGHT), WEIGHT_SUM, WAITING);
    }(),
    "the running thread is not part of the weighted mean");

} // namespace ours::task

#endif // #ifndef OURS_TASK_SCHEDULER_COMMON_HPP
