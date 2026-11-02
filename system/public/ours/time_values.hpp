/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_VALUES_HPP
#define OURS_TIME_VALUES_HPP 1

#include <ours/clock_page.hpp>
#include <ours/types.hpp>

#include <mtl/ratio.hpp>
#include <mtl/scale.hpp>
#include <ustl/limits.hpp>

namespace ours {
/// What the kernel publishes into the vDSO's read-only data, and what a read
/// of the monotonic clock scales a counter sample with.
///
/// The bytes are the anchor `time::Clock` pins a source switch at: reading
/// |ticks| on that source meant |nanoseconds| had elapsed, and one tick grows
/// the reading by |numerator| / |denominator| nanoseconds, rounded up. |seq|
/// is even while those words agree with each other. |accessible| is 1 only
/// when ring 3 can sample the same counter the anchor was pinned to.
/// |counter| is that sample's user virtual address, a 64-bit load; 0 if none.
///
/// The layout is the vDSO assembly's, which is why the offsets are asserted.
struct TimeValues {
    u64 magic;
    u64 seq;
    u64 ticks;
    i64 nanoseconds;
    u32 numerator;
    u32 denominator;
    u8 accessible;
    u8 pad[7];
    u64 counter;
};

inline constexpr auto TIME_VALUES_MAGIC = u64{0x0C10C1A11E000001};

static_assert(offsetof(TimeValues, magic) == 0);
static_assert(offsetof(TimeValues, seq) == 8);
static_assert(offsetof(TimeValues, ticks) == 16);
static_assert(offsetof(TimeValues, nanoseconds) == 24);
static_assert(offsetof(TimeValues, numerator) == 32);
static_assert(offsetof(TimeValues, denominator) == 36);
static_assert(offsetof(TimeValues, accessible) == 40);
static_assert(offsetof(TimeValues, counter) == 48);
static_assert(sizeof(TimeValues) == 56);

/// |counter| minus the anchor, scaled and added to the anchor's nanoseconds.
///
/// Unsigned subtraction, so a counter that has wrapped still yields the
/// delta. The scale is the one `ClockSource::ticks_to_time` uses, including
/// rounding up. A sum that does not fit in an `Instant` saturates at its
/// maximum.
[[nodiscard]]
inline auto monotonic_from(TimeValues const &values, u64 const counter) -> Instant {
    if (values.denominator == 0) {
        return values.nanoseconds;
    }
    auto const delta = counter - values.ticks;
    constexpr auto LIMIT = ustl::NumericLimits<u64>::max();
    auto const scaled = mtl::scale<mtl::RoundOption::Up, LIMIT, u64, u32>(delta, values.numerator,
                                                                          values.denominator);
    auto const sum = static_cast<__int128>(values.nanoseconds) + static_cast<__int128>(scaled);
    if (sum > ustl::NumericLimits<Instant>::max()) {
        return ustl::NumericLimits<Instant>::max();
    }
    return static_cast<Instant>(sum);
}
} // namespace ours

#endif // #ifndef OURS_TIME_VALUES_HPP
