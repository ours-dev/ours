/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/gaf.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/syscall.hpp>

#include <heap/kmalloc.hpp>
#include <heap/new.hpp>

namespace ours::object {
MessagePacket::MessagePacket(u8 *const data, u32 const num_bytes, Handle **const handles,
                             u32 const num_handles)
    : m_data(data)
    , m_num_bytes(num_bytes)
    , m_handles(handles)
    , m_num_handles(num_handles)
    , m_queue_hook() {}

/// A packet that was never read closes the handles it still carries. That can
/// release the last reference to what they name -- another channel end, with
/// packets of its own -- which is what Dispatcher::rc_recycle() is for: the
/// chain is walked as a loop, not down this stack. It runs outside every
/// channel lock (see the class comment), as closing a handle has to.
MessagePacket::~MessagePacket() {
    if (m_handles != nullptr) {
        for (u32 i = 0; i < m_num_handles; ++i) {
            if (m_handles[i] != nullptr) {
                auto closed = ustl::Box<Handle>::from_raw(m_handles[i]);
            }
        }
        heap::kfree(m_handles);
    }
    if (m_data != nullptr) {
        heap::kfree(m_data);
    }
}

auto MessagePacket::take_handles(ustl::Box<Handle> *const out) -> void {
    for (u32 i = 0; i < m_num_handles; ++i) {
        out[i] = ustl::Box<Handle>::from_raw(m_handles[i]);
        m_handles[i] = nullptr;
    }
}

auto MessagePacket::create(user_copy::UserInPtr<u8 const> const bytes, u32 const num_bytes,
                           ustl::Box<Handle> *const handles, u32 const num_handles,
                           ustl::Box<Self> *const out) -> Status {
    if (num_bytes > CHANNEL_MAX_MSG_BYTES || num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return Status::OutOfRange;
    }

    // A zero-byte message is legal and useful -- a pure wake-up -- and needs
    // no buffer at all.
    u8 *data = nullptr;
    if (num_bytes != 0) {
        data = static_cast<u8 *>(heap::kmalloc(num_bytes, mm::GAF_KERNEL));
        if (data == nullptr) {
            return Status::NoMemory;
        }
        if (auto const status = bytes.copy_array_from_user(data, num_bytes); status != Status::Ok) {
            heap::kfree(data);
            return status;
        }
    }

    // The array before the packet, and the handles moved into it only once
    // nothing can fail any more: until then they stay in |handles|, the
    // caller's, so a failure here closes them there and no handle is ever in
    // two places or none.
    Handle **array = nullptr;
    if (num_handles != 0) {
        array =
            static_cast<Handle **>(heap::kmalloc(num_handles * sizeof(Handle *), mm::GAF_KERNEL));
        if (array == nullptr) {
            if (data != nullptr) {
                heap::kfree(data);
            }
            return Status::NoMemory;
        }
    }

    auto *const packet = new (mm::GAF_KERNEL) Self(data, num_bytes, array, num_handles);
    if (packet == nullptr) {
        if (array != nullptr) {
            heap::kfree(array);
        }
        if (data != nullptr) {
            heap::kfree(data);
        }
        return Status::NoMemory;
    }
    for (u32 i = 0; i < num_handles; ++i) {
        DEBUG_ASSERT(handles[i] && handles[i]->table_koid() == 0,
                     "a handle in a message belongs to no table");
        array[i] = handles[i].into_raw();
    }
    *out = ustl::Box<Self>::from_raw(packet);
    return Status::Ok;
}

auto MessagePacket::copy_data_to(user_copy::UserOutPtr<u8> const bytes) const -> Status {
    if (m_num_bytes == 0) {
        return Status::Ok;
    }
    return bytes.copy_array_to_user(m_data, m_num_bytes);
}
} // namespace ours::object
