/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_CHANNEL_DISPATCHER_HPP
#define OURS_OBJECT_CHANNEL_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/message_packet.hpp>

#include <gktl/canary.hpp>
#include <ustl/box.hpp>

namespace ours::object {
/// Zircon ZX_DEFAULT_CHANNEL_RIGHTS: no Duplicate, so that a channel end has
/// exactly one holder unless someone deliberately mints a second.
constexpr auto RIGHTS_DEFAULT_CHANNEL =
    (Rights::Basic & ~Rights::Duplicate) | Rights::Io | Rights::Signal | Rights::SignalPeer;

/// How many messages may wait on one end before a write to it is refused.
///
/// Kernel policy, not ABI: it bounds how much kernel memory one writer can
/// pin in a reader that never reads. Full answers NoResource and leaves
/// WRITABLE set (design decision D11) -- WRITABLE says only that the peer is
/// alive, and clearing it for back-pressure would change what it means. Zircon
/// raises a policy exception at its limit instead; that waits for there to be
/// an exception mechanism.
constexpr u32 CHANNEL_MAX_PENDING_MESSAGES = 64;

using ChannelDispatcherBase =
    PeeredDispatcher<ChannelDispatcher, RIGHTS_DEFAULT_CHANNEL, Signals::None>;

/// A bidirectional message pipe: two ends, each with a queue of messages
/// written by the other.
///
/// Signals, per end:
///   Readable    -- this end's queue is not empty
///   Writable    -- the peer is alive (not back-pressure; see
///                  CHANNEL_MAX_PENDING_MESSAGES)
///   PeerClosed  -- the peer lost its last handle; set once, never cleared
///
/// Zircon counterpart: ChannelDispatcher (kernel/object/channel_dispatcher.cc).
class ChannelDispatcher final: public ChannelDispatcherBase {
    using Base = ChannelDispatcherBase;
    using Self = ChannelDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Channel);

    /// Makes the two ends of a new channel, both Writable.
    ///
    /// |rights| receives the rights each end's first handle should carry. It
    /// used to be declared as a Signals out-parameter, which was a slip: what
    /// a factory reports beside its objects is the rights for them, as every
    /// other create() here does.
    static auto create(KernelHandle<Self> *handle0, KernelHandle<Self> *handle1, Rights *rights)
        -> Status;

    /// Queues |message| on the peer and raises the peer's Readable.
    ///
    /// PeerClosed when there is no peer to write to; NoResource when its
    /// queue is full. The message is consumed either way: a caller retrying
    /// after NoResource makes a new one.
    auto write(ustl::Box<MessagePacket> message) -> Status;

    /// Takes the message at the head of this end's queue, provided it fits in
    /// |max_bytes| and |max_handles|.
    ///
    /// ShouldWait when the queue is empty and the peer alive, PeerClosed when
    /// it is empty and the peer gone -- a closed peer's last messages are
    /// still delivered first. BufferTooSmall when the head message has more
    /// bytes than |max_bytes| or more handles than |max_handles|; it stays
    /// where it is, and |actual_bytes| and |actual_handles| say how big it
    /// is, so the caller can retry with larger buffers.
    auto read(u32 max_bytes, u32 max_handles, u32 *actual_bytes, u32 *actual_handles,
              ustl::Box<MessagePacket> *out) -> Status;

    /// PeeredDispatcher's hooks, called under the shared lock.
    auto on_zero_handles_locked() -> void;
    auto on_peer_zero_handles_locked() -> void;

    ~ChannelDispatcher() override;

  private:
    template <typename, Rights, Signals>
    friend class PeeredDispatcher;

    static constexpr auto MAGIC = gktl::magic("ChannelDispatcher");

    explicit ChannelDispatcher(ustl::Arc<PeerHolder> holder);

    /// Queues a message the peer wrote, under the shared lock.
    ///
    /// By reference, and it takes the packet only on success: a message
    /// refused for a full queue stays with the caller's Box and is freed when
    /// write() returns -- after the lock -- rather than here, under it.
    auto write_self_locked(ustl::Box<MessagePacket> &message) -> Status;

    /// Messages the peer wrote and this end has not read yet. Guarded by
    /// peer_lock().
    MessagePacket::List m_messages;
    u32 m_message_count = 0;

    /// Set when the peer loses its last handle, never cleared. Guarded by
    /// peer_lock().
    bool m_peer_has_closed = false;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_CHANNEL_DISPATCHER_HPP
