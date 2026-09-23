/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_OBJECT_HPP
#define OU_OBJECT_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Waits until the object |handle| names asserts any of |signals|.
///
/// |deadline| is 0 to poll or INSTANT_INFINITE to wait as long as it takes;
/// nothing in between is supported until a program can read a clock. The
/// signals asserted when the wait ended go to |observed| (which may be null).
///
/// Ok when one of |signals| is asserted, TimeOut when a poll found none,
/// Cancelled when |handle| was closed under the wait -- |observed| then
/// includes Signals::HandleClosed.
auto object_wait_one(ours::HandleValue handle, ours::Signals signals, ours::Instant deadline,
                     ours::Signals *observed) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_OBJECT_HPP
