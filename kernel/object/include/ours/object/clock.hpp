/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_CLOCK_HPP
#define OURS_OBJECT_CLOCK_HPP 1

#include <ours/types.hpp>
#include <ours/version.hpp>

namespace ours::object {
/// Nanoseconds since boot on the monotonic clock: the reading
/// `time::SteadyClock` gives the rest of the kernel.
///
/// The system-call layer asks here and does not name a clock. The reading
/// has one owner, so a later publication of it cannot disagree with this.
[[nodiscard]]
auto monotonic_now() -> Instant;

[[nodiscard]]
auto ticks_per_second() -> u64;

[[nodiscard]]
auto boot_offset() -> i64;

[[nodiscard]]
auto published_version() -> u64;

/// The kernel version string. Null from the trap: the bytes a program reads
/// are in the vDSO, and this only exists so the trap stub links.
[[nodiscard]]
auto kernel_version() -> KernelVersion;
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_CLOCK_HPP
