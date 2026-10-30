/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICKS_PER_SECOND_HPP
#define OURS_TIME_TICKS_PER_SECOND_HPP 1

#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

#include <ffl/saturating.hpp>

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
/// ticks_per_second()'s callers may rely on not to change. A debug build
/// asserts; any build keeps the first rate and ignores the later one.
auto set_ticks_per_second(u64 rate) -> void;

/// How many counts of arch::Tick pass in |duration| at the counter's rate; 0
/// while ticks_per_second() is 0, and 0 for a duration that is not positive.
///
/// A wait is written as a time and turned into counts here, because a count
/// is a different wait on each counter: 1 GHz under QEMU's x86, a few GHz on
/// a real PC, 10 MHz on QEMU's RISC-V board.
///
/// The 0 for an unknown rate is the absence of an answer, not a short wait,
/// and it is for each caller to say what it means. A bound of 0 counts ends
/// the moment it starts, which is right where no wait could help (logz4's
/// panic lock: only the boot CPU runs, so a lock it finds held is its own)
/// and wrong where the wait is the point, which has to name the count it
/// falls back on (the lockup reporter's wait for the other CPUs, the PC
/// reset's settle time).
///
/// The count is the rate in whole kilohertz times the milliseconds, so a rate
/// that is not a multiple of 1 kHz is rounded down first, and the count falls
/// short by less than one count per millisecond of |duration|: under 0.1% for
/// a counter of 1 MHz or faster. A nominal 10 MHz or 1 GHz loses nothing; a
/// rate that a calibration measured loses its odd hertz, 45 of the
/// 999 995 045 Hz that QEMU's x86 gives (45 parts per billion). For the same
/// reason a rate under 1 kHz answers 0 too; no counter this kernel runs on is
/// that slow.
///
/// It saturates: a product that does not fit in 64 bits answers the largest
/// u64, never a wrapped number, so a longer duration is never fewer counts. It
/// does not reach that for anything a caller asks. At 10 GHz a u64 holds 1.8e12
/// ms, about 58 years; a minute's wait is exact up to a rate of 3e17 Hz, and a
/// two-second one up to 9e18 Hz. A bound that did saturate is a wait that never
/// ends, so a caller that takes its duration from outside the code has to
/// limit it first.
///
/// Like ticks_per_second(), it takes no lock, allocates nothing and reads no
/// CPU-local state, so it may be called from an NMI or a panic.
[[nodiscard]]
inline auto ticks_in(Milliseconds const duration) -> u64 {
    constexpr auto MILLISECONDS_PER_SECOND = u64{1'000};
    if (duration <= Milliseconds::zero()) {
        return 0;
    }
    auto const per_millisecond = ticks_per_second() / MILLISECONDS_PER_SECOND;
    return ffl::saturating_mul(per_millisecond, static_cast<u64>(duration.count()));
}
} // namespace ours::time

#endif // #ifndef OURS_TIME_TICKS_PER_SECOND_HPP
