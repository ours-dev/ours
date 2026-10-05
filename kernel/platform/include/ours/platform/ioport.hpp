/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_IOPORT_HPP
#define OURS_PLATFORM_IOPORT_HPP 1

#include <ours/types.hpp>

#include <ustl/views/span.hpp>

namespace ours::platform {
/// A run of I/O ports, [base, base + count).
struct IoPortRange {
    u16 base;
    u16 count;
};

/// The I/O ports the kernel drives itself, which no I/O port resource grants,
/// not even the root one: a program writing to one of them would change a
/// device's state behind the back of the kernel driver that keeps it (the
/// PIT's tick, the PICs' masks, the console's line settings).
///
/// The list is fixed when the kernel is built, sorted by base, and its ranges
/// do not overlap. A platform without I/O ports answers an empty list.
[[nodiscard]]
auto kernel_ioports() -> ustl::views::Span<IoPortRange const>;
} // namespace ours::platform

#endif // #ifndef OURS_PLATFORM_IOPORT_HPP
