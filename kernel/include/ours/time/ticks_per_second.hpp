/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICKS_PER_SECOND_HPP
#define OURS_TIME_TICKS_PER_SECOND_HPP 1

#include <ours/types.hpp>

namespace ours::time {
/// The rate at which arch::Tick::get() counts, in counts per second, or 0
/// while it is not known yet. Once it answers non-zero the answer does not
/// change.
///
/// This is the rate of the CPU's own counter. It is not HZ (cfg.hpp), the
/// rate of the periodic tick device, and it is not any ClockSource's rate.
///
/// It is what turns a span measured in arch::Tick counts into a time. A
/// caller that may run before the rate is known -- a panic in early boot --
/// has to have an answer for 0.
///
/// The kernel's console logger (kernel/lib/logz4) calls it for every line it
/// writes, with interrupts off, from an NMI or a panic, and from the first
/// line of the boot, before CPU-local storage exists. So it takes no lock,
/// allocates nothing and reads no CPU-local state.
///
/// The layer that learns the rate calls set_ticks_per_second(), and does so
/// before any CPU but the boot CPU starts: while this answers 0, only the
/// boot CPU runs. The one exception is a platform that could not learn the
/// rate, where it answers 0 for good (a PC whose local APIC timer
/// calibration failed).
[[nodiscard]]
auto ticks_per_second() -> u64;

/// Records the rate ticks_per_second() answers. Called once, by whichever
/// layer learns it: on a PC, the boot CPU's local APIC timer calibration,
/// which measures the TSC against the clock source; on a platform with a
/// device tree, the reader of its `timebase-frequency`.
///
/// |rate| is not 0, and a second call is a bug: the answer is the one thing
/// ticks_per_second()'s callers may rely on not to change.
auto set_ticks_per_second(u64 rate) -> void;
} // namespace ours::time

#endif // #ifndef OURS_TIME_TICKS_PER_SECOND_HPP
