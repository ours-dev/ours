/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_HANDLE_HPP
#define OU_HANDLE_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Closes |handle|: this process stops holding the object it names, and when
/// it was the last holder anywhere the object learns so -- a channel's peer
/// sees PEER_CLOSED, for instance.
///
/// HANDLE_INVALID is accepted and does nothing, so a cleanup path may close
/// every slot it has without first asking which ones were filled. Any other
/// value that names nothing here answers BadHandle, and that includes a value
/// that was valid once and has already been closed.
auto handle_close(ours::HandleValue handle) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_HANDLE_HPP
