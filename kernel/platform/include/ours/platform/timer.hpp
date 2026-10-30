/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_TIMER_HPP
#define OURS_PLATFORM_TIMER_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
auto current_mono_ticks() -> Ticks;

auto current_mono_time() -> Time;

auto platform_convert_mono_time_to_ticks(Time timepoint) -> Ticks;

auto platform_set_oneshot_timer(Ticks deadline) -> Status;

/// Unit: ms
auto get_periodic_time() -> usize;

/// The rate at which arch::Tick::get() counts, in counts per second, or 0
/// while the platform does not know it yet. Once it answers non-zero the
/// answer does not change.
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
/// A platform that can measure the rate knows it before any CPU but the boot
/// CPU starts: while it answers 0, only the boot CPU runs. A platform that
/// could not measure it answers 0 for good (a PC whose local APIC timer
/// calibration failed).
[[nodiscard]]
auto arch_tick_hz() -> u64;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_TIMER_HPP