/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/object/clock.hpp>
#include <ours/time/clock.hpp>

namespace ours::object {
auto monotonic_now() -> Instant {
    return time::SteadyClock::now().time_since_epoch().count();
}
} // namespace ours::object
