/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_CHANNEL_HPP
#define OU_CHANNEL_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Makes a channel: two ends, each a handle in this process's table. What is
/// written to one end is read from the other.
auto channel_create(ours::HandleValue *out0, ours::HandleValue *out1,
                    ours::ChannelCreateOptions options = ours::ChannelCreateOptions::None)
    -> ours::Status;

/// Queues |num_bytes| of |bytes| as one message on |channel|'s other end.
///
/// At most CHANNEL_MAX_MSG_BYTES (OutOfRange past it). PeerClosed when the
/// other end is gone; NoResource when its queue is full. Handles in messages
/// are not supported yet, which is why this entry does not take any.
auto channel_write(ours::HandleValue channel, void const *bytes, u32 num_bytes) -> ours::Status;

/// Takes the next message on |channel| into |bytes|, and its length into
/// |actual_bytes| (which may be null).
///
/// Does not block: ShouldWait when nothing has arrived yet -- wait for
/// Signals::Readable with object_wait_one() and try again. PeerClosed when
/// nothing has arrived and nothing will. BufferTooSmall when the message is
/// longer than |num_bytes|: it is left where it is and |actual_bytes| says
/// how long it is.
auto channel_read(ours::HandleValue channel, void *bytes, u32 num_bytes, u32 *actual_bytes)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_CHANNEL_HPP
