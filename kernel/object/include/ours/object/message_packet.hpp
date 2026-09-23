/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_MESSAGE_PACKET_HPP
#define OURS_OBJECT_MESSAGE_PACKET_HPP 1

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
/// Bytes only for now; carrying handles is P1. The header and the bytes are
/// two allocations, the bytes at most CHANNEL_MAX_MSG_BYTES -- one kmalloc, so
/// that nothing here depends on vmalloc, whose regions are few and whose
/// vfree has no TLB shootdown yet. Zircon's MessagePacket chains fixed-size
/// buffers instead; that is the P1 shape, and it will lift the limit to 64 KiB.
class MessagePacket final {
    using Self = MessagePacket;

  public:
    /// A packet holding |num_bytes| copied from the user buffer |bytes|.
    ///
    /// OutOfRange past CHANNEL_MAX_MSG_BYTES, InvArgs when the buffer is not
    /// the caller's to read, NoMemory when the kernel has none.
    static auto create(user_copy::UserInPtr<u8 const> bytes, u32 num_bytes, ustl::Box<Self> *out)
        -> Status;

    ~MessagePacket();

    [[nodiscard]]
    auto num_bytes() const -> u32 {
        return m_num_bytes;
    }

    /// Copies the whole message to the user buffer |bytes|, which the caller
    /// has checked is at least num_bytes() long.
    [[nodiscard]]
    auto copy_data_to(user_copy::UserOutPtr<u8> bytes) const -> Status;

    USTL_NOCOPYABLE(MessagePacket);

  private:
    MessagePacket(u8 *data, u32 num_bytes);

    u8 *m_data;
    u32 m_num_bytes;

    ustl::collections::intrusive::ListMemberHook<> m_queue_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_queue_hook, QueueOption);

  public:
    USTL_DECLARE_LIST(Self, List, QueueOption);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_MESSAGE_PACKET_HPP
