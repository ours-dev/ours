/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/clock.hpp>

namespace ours::syscall {
auto clock_get_monotonic() -> Instant {
    return object::monotonic_now();
}
} // namespace ours::syscall
