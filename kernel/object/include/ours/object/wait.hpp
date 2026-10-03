/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_WAIT_HPP
#define OURS_OBJECT_WAIT_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::object {
/// Waits until the object |handle| names asserts any of |signals|, the handle
/// leaves the caller's table, or |deadline| passes, and reports the signals
/// asserted when the wait ended in |observed|.
///
/// Level-triggered: a condition that already holds returns at once. Ok when a
/// signal matched, TimeOut when the deadline passed first, Cancelled when the
/// handle left the table under the wait -- closed, or consumed by a call such
/// as channel_write; HandleTable::unlink_locked() cancels either way -- and
/// |observed| then includes HandleClosed. The handle must grant Wait.
///
/// |deadline| is 0 for a poll or INSTANT_INFINITE; any other value, a negative
/// one included, is NotSupported for now, because a user program cannot read
/// the clock yet and so has no way to compute one.
///
/// This is the whole of the waiting machinery, so that lib/syscall only has
/// to move arguments: attach an observer under the handle table's lock, sleep
/// on an event it signals, detach, read what is asserted. Zircon's
/// sys_object_wait_one() with WaitSignalObserver.
auto wait_one(ours::Uandle handle, Signals signals, Instant deadline, Signals *observed) -> Status;
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_WAIT_HPP
