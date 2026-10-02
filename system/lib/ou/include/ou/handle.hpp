/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_HANDLE_HPP
#define OU_HANDLE_HPP 1

#include <ours/rights.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Closes |handle|: it leaves this process's table, and when it was the last
/// handle to its object anywhere the object learns so -- a channel's peer
/// sees PEER_CLOSED, for instance.
///
/// HANDLE_INVALID is accepted and does nothing, so a cleanup path may close
/// every slot it has without first asking which ones were filled. Any other
/// value that names nothing here answers BadHandle, and that includes a value
/// that was valid once and has already been closed.
auto handle_close(ours::HandleValue handle) -> ours::Status;

/// Makes a second handle to the object |handle| names, with |rights|, and
/// puts its value in |out|. Zircon zx_handle_duplicate().
///
/// |handle| needs Duplicate: AccessDenied otherwise -- a channel, for one,
/// is never made with it. |rights| is Rights::SameRights for the same rights
/// as |handle|, or any subset of them; a right |handle| does not have is
/// InvArgs. A duplicate is the one way to make a second handle to an object
/// -- a channel_write moves the handles it sends rather than copying them --
/// and it must never be a way to widen one. Narrowing is what it is for:
/// keep the original, and send a copy that can read but not write -- it
/// still needs Transfer, or it cannot be sent at all.
///
/// The new handle is this process's, charged to its quota (NoResource when
/// that is used up, or when the system-wide handle arena is full). |handle|
/// is untouched either way.
auto handle_duplicate(ours::HandleValue handle, ours::Rights rights, ours::HandleValue *out)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_HANDLE_HPP
