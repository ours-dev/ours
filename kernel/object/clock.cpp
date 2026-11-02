/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/object/clock.hpp>
#include <ours/time/clock.hpp>
#include <ours/version.hpp>

namespace ours::object {
auto monotonic_now() -> Instant {
    return time::SteadyClock::now().time_since_epoch().count();
}

auto ticks_per_second() -> u64 {
    return time::SteadyClock::reading().frequency_hz;
}

auto boot_offset() -> i64 {
    return time::SystemClock::boot_offset();
}

auto published_version() -> u64 {
    return 1;
}

auto kernel_version() -> KernelVersion {
    return nullptr;
}
} // namespace ours::object
