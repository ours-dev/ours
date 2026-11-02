/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The clock calls: the kernel's monotonic time, handed to user space.
#include <syscall/handlers.hpp>

#include <ours/time/clock.hpp>

namespace ours::syscall {

/// Reads the steady clock: nanoseconds since the system started counting.
/// No resource and no arguments -- the value is the answer, and the call
/// cannot fail while a clock source is running. The cheapest call in the
/// table on purpose: timing a microsecond-scale operation must not time
/// the reading.
auto clock_get_monotonic(user_copy::UserOutPtr<ours::Instant> out) -> Status {
    Instant const now = time::SteadyClock::now().time_since_epoch().count();
    if (out.is_null()) {
        return Status::Ok;
    }
    return out.copy_to_user(now);
}

} // namespace ours::syscall
