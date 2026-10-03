/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/channel_dispatcher.hpp>

#include <gktl/counter.hpp>
#include <heap/new.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(DISPATCHER_CHANNEL_CREATE_COUNT, "dispatcher.channel.create");

namespace ours::object {
ChannelDispatcher::ChannelDispatcher(ustl::Arc<PeerHolder> holder) : Base(ustl::move(holder)) {}

ChannelDispatcher::~ChannelDispatcher() {
    // Whatever was never read. This runs when the last reference goes -- after
    // both the handles and the peer's link are gone -- so no lock is held and
    // none is needed: nothing else can reach the queue any more.
    while (auto front = m_messages.front()) {
        auto &message = *front;
        auto _ = m_messages.erase(m_messages.iterator_to(message));
        auto owned = ustl::Box<MessagePacket>::from_raw(&message);
    }
}

auto ChannelDispatcher::create(KernelHandle<Self> *const handle0, KernelHandle<Self> *const handle1,
                               Rights *const rights) -> Status {
    auto holder = ustl::Arc<PeerHolder>::adopt(new (mm::GAF_KERNEL) PeerHolder());
    if (!holder) {
        return Status::NoMemory;
    }
    auto end0 = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(holder));
    if (!end0) {
        return Status::NoMemory;
    }
    auto end1 = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(holder));
    if (!end1) {
        return Status::NoMemory;
    }

    // Both links before either end is reachable from anywhere, which is what
    // lets init_peer() run without the lock.
    end0->init_peer(end1);
    end1->init_peer(end0);

    // Writable from birth: each end's peer exists. No observer can be
    // attached yet, so this notifies nobody.
    end0->update_state(Signals::None, Signals::Writable);
    end1->update_state(Signals::None, Signals::Writable);

    *handle0 = KernelHandle<Self>(ustl::move(end0));
    *handle1 = KernelHandle<Self>(ustl::move(end1));
    *rights = default_right();

    DISPATCHER_CHANNEL_CREATE_COUNT.add(1);
    return Status::Ok;
}

auto ChannelDispatcher::write(ustl::Box<MessagePacket> message) -> Status {
    verify_canary();

    // |message| outlives the guard below -- a parameter is destroyed after
    // the function's locals -- so a packet that is not queued is freed only
    // after the shared lock is released.
    Guard guard{peer_lock()};
    auto &peer = this->peer();
    if (!peer) {
        return Status::PeerClosed;
    }
    return peer->write_self_locked(message);
}

auto ChannelDispatcher::write_self_locked(ustl::Box<MessagePacket> &message) -> Status {
    if (m_message_count >= CHANNEL_MAX_PENDING_MESSAGES) {
        return Status::NoResource;
    }
    m_messages.push_back(*message.into_raw());
    m_message_count += 1;

    // Holder lock, then this end's own lock inside update_state(): the fixed
    // order between the two.
    update_state(Signals::None, Signals::Readable);
    return Status::Ok;
}

auto ChannelDispatcher::read(u32 const max_bytes, u32 const max_handles, u32 *const actual_bytes,
                             u32 *const actual_handles, ustl::Box<MessagePacket> *const out)
    -> Status {
    verify_canary();

    Guard guard{peer_lock()};
    auto front = m_messages.front();
    if (!front) {
        *actual_bytes = 0;
        *actual_handles = 0;
        if (m_peer_has_closed) {
            return Status::PeerClosed;
        }
        return Status::ShouldWait;
    }

    auto &message = *front;
    // Both sizes, whether or not the message fits, so that a caller whose
    // buffers were too small learns how big to come back with. Either one
    // too small leaves the message where it is, at the front: taking the
    // bytes and dropping the handles, or the other way round, would lose
    // part of a message nobody asked to lose. Zircon's ReadMsg does the same.
    *actual_bytes = message.num_bytes();
    *actual_handles = message.num_handles();
    if (message.num_bytes() > max_bytes || message.num_handles() > max_handles) {
        return Status::BufferTooSmall;
    }

    auto _ = m_messages.erase(m_messages.iterator_to(message));
    m_message_count -= 1;
    if (m_message_count == 0) {
        update_state(Signals::Readable, Signals::None);
    }

    // Copied out and freed by the caller, after this lock is released.
    *out = ustl::Box<MessagePacket>::from_raw(&message);
    return Status::Ok;
}

auto ChannelDispatcher::on_zero_handles_locked() -> void {
    // Nothing to do for this end itself. Its unread messages stay queued until
    // the object is destroyed -- which happens once the peer's link to it is
    // cut (PeeredDispatcher::on_zero_handles) and the last Arc goes -- so that
    // they are freed outside any lock rather than under this one.
}

auto ChannelDispatcher::on_peer_zero_handles_locked() -> void {
    // Zircon's OnPeerZeroHandlesLocked(): the peer is gone for good. Readers
    // may still drain what it wrote; writers here have nowhere to write.
    m_peer_has_closed = true;
    update_state(Signals::Writable, Signals::PeerClosed);
}
} // namespace ours::object
