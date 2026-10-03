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

/// Queues |num_bytes| of |bytes| and the |num_handles| handles of |handles|
/// as one message on |channel|'s other end.
///
/// At most CHANNEL_MAX_MSG_BYTES bytes and CHANNEL_MAX_MSG_HANDLES handles
/// (OutOfRange past either). PeerClosed when the other end is gone;
/// NoResource when its queue is full.
///
/// Every handle named leaves this process's table, whatever the answer: sent
/// with the message, or closed if the write fails. The exceptions are more
/// than CHANNEL_MAX_MSG_HANDLES handles (OutOfRange) and a |handles| that
/// cannot be read (InvArgs): answered before any handle is touched, they
/// leave every one in the table, still the caller's to close. Each needs
/// Transfer (AccessDenied), none may be |channel| itself (NotSupported), and
/// a value that names nothing -- or one already named earlier in the array --
/// is BadHandle. After any other answer a caller must not use the values.
///
/// The handle parameters default to none, for the common message of bytes
/// alone; this is still the one entry for channel_write, with arguments a
/// caller may leave out. |options| must be None (InvArgs otherwise); it is
/// passed through as given, so that a caller can check the kernel refuses
/// anything else.
auto channel_write(ours::HandleValue channel, void const *bytes, u32 num_bytes,
                   ours::HandleValue const *handles = nullptr, u32 num_handles = 0,
                   ours::ChannelWriteOptions options = ours::ChannelWriteOptions::None)
    -> ours::Status;

/// Takes the next message on |channel|: its bytes into |bytes| and its length
/// into |actual_bytes|, its handles into |handles| and their number into
/// |actual_handles|. Either count may be null.
///
/// Does not block: ShouldWait when nothing has arrived yet -- wait for
/// Signals::Readable with object_wait_one() and try again. PeerClosed when
/// nothing has arrived and nothing will. BufferTooSmall when the message has
/// more bytes than |num_bytes| or more handles than |num_handles|: it is left
/// where it is, and the counts say how much room it needs.
///
/// The handles received are this process's from then on: new values in its
/// table, which it must close or pass on. The handle parameters default to
/// none, for a reader that expects bytes alone -- a message that does carry
/// handles is then BufferTooSmall rather than having them dropped. |options|
/// must be None (InvArgs otherwise), and is passed through as given.
auto channel_read(ours::HandleValue channel, void *bytes, u32 num_bytes, u32 *actual_bytes,
                  ours::HandleValue *handles = nullptr, u32 num_handles = 0,
                  u32 *actual_handles = nullptr,
                  ours::ChannelReadOptions options = ours::ChannelReadOptions::None)
    -> ours::Status;
} // namespace ou

#endif // #ifndef OU_CHANNEL_HPP
