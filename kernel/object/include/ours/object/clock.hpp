/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_CLOCK_HPP
#define OURS_OBJECT_CLOCK_HPP 1

#include <ours/types.hpp>

namespace ours::object {
/// Nanoseconds since boot on the monotonic clock: the reading
/// `time::SteadyClock` gives the rest of the kernel.
///
/// The system-call layer asks here and does not name a clock. The reading
/// has one owner, so a later publication of it cannot disagree with this.
[[nodiscard]]
auto monotonic_now() -> Instant;
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_CLOCK_HPP
