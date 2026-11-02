/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/clock.hpp>

namespace ours::syscall {
auto clock_get_monotonic() -> Instant {
    return object::monotonic_now();
}

auto ticks_per_second() -> u64 {
    return object::ticks_per_second();
}

auto boot_offset() -> i64 {
    return object::boot_offset();
}

auto published_version() -> u64 {
    return object::published_version();
}

auto kernel_version() -> KernelVersion {
    return object::kernel_version();
}
} // namespace ours::syscall
