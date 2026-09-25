/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_MESSAGE_PACKET_HPP
#define OURS_OBJECT_MESSAGE_PACKET_HPP 1

#include <ours/object/handle.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <user_copy/user_copy.hpp>
#include <ustl/box.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
/// One channel message, in kernel memory.
///
/// A message is copied in from the writer *before* the channel's lock is
/// taken, and out to the reader *after* it is released: a user copy can
/// fault, and the fault path must never find a channel lock held. So the
/// packet is the unit that crosses the lock -- built outside it, queued and
/// dequeued inside it, copied out and freed outside it again.
///
/// It can carry handles: a handle written into a channel leaves the writer's
/// table and belongs to the packet until a reader takes it into its own. In
/// transit it is in no table at all -- its table koid is 0, so no process's
/// values resolve to it -- and if nobody ever reads it, the packet closes it
/// when the packet goes. Zircon's MessagePacket holds its handles the same
/// way, in an array the packet owns.
///
/// Header, bytes and handle array are separate allocations, the bytes at most
/// CHANNEL_MAX_MSG_BYTES and the handles at most CHANNEL_MAX_MSG_HANDLES --
/// kmalloc each, so that nothing here depends on vmalloc, whose regions are
/// few and whose vfree has no TLB shootdown yet. Zircon's MessagePacket
/// chains fixed-size buffers instead; that shape comes with the 64 KiB
/// limit.
class MessagePacket final {
    using Self = MessagePacket;

  public:
    /// A packet holding |num_bytes| copied from the user buffer |bytes|, and
    /// the |num_handles| handles in |handles|.
    ///
    /// The handles are moved into the packet only when it is made; on any
    /// failure they are left in |handles|, still the caller's, which closes
    /// them by letting them go -- a handle handed to a write is consumed
    /// whatever the write does. Each must already be out of every table.
    ///
    /// OutOfRange past CHANNEL_MAX_MSG_BYTES or CHANNEL_MAX_MSG_HANDLES,
    /// InvArgs when the buffer is not the caller's to read, NoMemory when the
    /// kernel has none.
    static auto create(user_copy::UserInPtr<u8 const> bytes, u32 num_bytes,
                       ustl::Box<Handle> *handles, u32 num_handles, ustl::Box<Self> *out) -> Status;

    ~MessagePacket();

    [[nodiscard]]
    auto num_bytes() const -> u32 {
        return m_num_bytes;
    }

    [[nodiscard]]
    auto num_handles() const -> u32 {
        return m_num_handles;
    }

    /// Moves every handle out of the packet into |out|, which has room for
    /// num_handles(); the packet is left with none, and closes none when it
    /// goes.
    auto take_handles(ustl::Box<Handle> *out) -> void;

    /// Copies the whole message to the user buffer |bytes|, which the caller
    /// has checked is at least num_bytes() long.
    [[nodiscard]]
    auto copy_data_to(user_copy::UserOutPtr<u8> bytes) const -> Status;

    USTL_NOCOPYABLE(MessagePacket);

  private:
    MessagePacket(u8 *data, u32 num_bytes, Handle **handles, u32 num_handles);

    u8 *m_data;
    u32 m_num_bytes;

    /// Owned: each entry is a handle in transit, or null once taken. Raw
    /// pointers rather than Box<Handle>, because the array is a kmalloc
    /// buffer with no constructors run; the destructor turns each one back
    /// into a Box to close it.
    Handle **m_handles;
    u32 m_num_handles;

    ustl::collections::intrusive::ListMemberHook<> m_queue_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_queue_hook, QueueOption);

  public:
    USTL_DECLARE_LIST(Self, List, QueueOption);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_MESSAGE_PACKET_HPP
