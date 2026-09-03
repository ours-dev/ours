/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CFG_HPP
#define OURS_TIME_CFG_HPP 1

#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

/// The build may pin the tick rate; 100 Hz otherwise. This one stays a macro
/// because it is a build knob that has to be settable from the command line,
/// which is the only thing macros are kept for in this tree.
#ifndef OURS_CONFIG_HZ
#define OURS_CONFIG_HZ 100
#endif // #ifndef OURS_CONFIG_HZ

namespace ours::time {
/// Ticks per second, Linux's HZ.
///
/// A constant in ours::time rather than a bare `#define HZ`, which this used
/// to be -- and which was written above the includes, so every header pulled
/// in afterwards saw a two-letter macro claiming the global namespace.
///
/// Typed as Duration::rep, not usize, so that dividing a duration by it stays
/// a duration of the same representation: `Nanoseconds / u64` would deduce
/// the common type of int64_t and uint64_t, which is uint64_t, and quietly
/// give NSEC_PER_TICK a different type from Duration.
constexpr auto HZ = static_cast<Duration::rep>(OURS_CONFIG_HZ);

/// How long one tick lasts.
constexpr auto NSEC_PER_TICK = duration_cast<Nanoseconds>(Seconds(1)) / HZ;

static_assert(NSEC_PER_TICK > Duration::zero(), "HZ is too high for a nanosecond tick");

} // namespace ours::time

#endif // #ifndef OURS_TIME_CFG_HPP
