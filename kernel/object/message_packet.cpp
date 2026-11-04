/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/channel.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/syscall.hpp>

#include <cstring>

#include <heap/kmalloc.hpp>
#include <heap/new.hpp>

namespace ours::object {
// The bytes and the handle slots are vectors, whose allocator is `new`, and
// `new` goes to vmalloc above KMALLOC_MAX_SIZE. The limits keep both under it;
// an array of objects with destructors carries a one-word count in front.
static_assert(CHANNEL_MAX_MSG_BYTES <= heap::KMALLOC_MAX_SIZE);
static_assert(CHANNEL_MAX_MSG_HANDLES * sizeof(ustl::Box<Handle>) + sizeof(usize) <=
              heap::KMALLOC_MAX_SIZE);

MessagePacket::MessagePacket(ktl::Vec<u8> data, ktl::Vec<ustl::Box<Handle>> handles)
    : m_data(ustl::move(data)), m_handles(ustl::move(handles)), m_queue_hook() {}

/// A packet that was never read closes the handles it still carries. That can
/// release the last reference to what they name -- another channel end, with
/// packets of its own -- which is what Dispatcher::rc_recycle() is for: the
/// chain is walked as a loop, not down this stack. It runs outside every
/// channel lock (see the class comment), as closing a handle has to.
MessagePacket::~MessagePacket() = default;

auto MessagePacket::take_handles(ustl::Box<Handle> *const out) -> void {
    for (u32 i = 0; i < num_handles(); ++i) {
        out[i] = ustl::move(m_handles[i]);
    }
}

auto MessagePacket::create(user_copy::UserInPtr<u8 const> const bytes, u32 const num_bytes,
                           ustl::Box<Handle> *const handles, u32 const num_handles,
                           ustl::Box<Self> *const out) -> Status {
    if (num_bytes > CHANNEL_MAX_MSG_BYTES || num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return Status::OutOfRange;
    }

    // A zero-byte message is legal and useful -- a pure wake-up -- and needs
    // no buffer at all: resizing an empty vector to 0 allocates nothing.
    auto data = ktl::Vec<u8>{};
    if (data.resize(num_bytes).is_err()) {
        return Status::NoMemory;
    }
    if (num_bytes != 0) {
        if (auto const status = bytes.copy_array_from_user(data.data(), num_bytes);
            status != Status::Ok) {
            return status;
        }
    }
    return assemble(ustl::move(data), handles, num_handles, out);
}

auto MessagePacket::create(std::span<u8 const> const bytes, ustl::Box<Handle> *const handles,
                           u32 const num_handles, ustl::Box<Self> *const out) -> Status {
    // Checked before the narrowing below, so a span longer than a u32 cannot
    // wrap into a small, acceptable size.
    if (bytes.size() > CHANNEL_MAX_MSG_BYTES || num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return Status::OutOfRange;
    }
    auto const num_bytes = static_cast<u32>(bytes.size());

    auto data = ktl::Vec<u8>{};
    if (data.resize(num_bytes).is_err()) {
        return Status::NoMemory;
    }
    if (num_bytes != 0) {
        std::memcpy(data.data(), bytes.data(), num_bytes);
    }
    return assemble(ustl::move(data), handles, num_handles, out);
}

auto MessagePacket::assemble(ktl::Vec<u8> data, ustl::Box<Handle> *const handles,
                             u32 const num_handles, ustl::Box<Self> *const out) -> Status {
    // The slots before the packet, and the handles moved into them only once
    // nothing can fail any more: until then they stay in |handles|, the
    // caller's, so a failure here closes them there and no handle is ever in
    // two places or none. A failure frees whatever was allocated with the
    // vectors that hold it.
    auto slots = ktl::Vec<ustl::Box<Handle>>{};
    if (slots.resize(num_handles).is_err()) {
        return Status::NoMemory;
    }

    // If the allocation fails the constructor never runs, so |data| and
    // |slots| are still this function's, and go with it.
    auto *const packet = new (mm::ALLOC_KERNEL) Self(ustl::move(data), ustl::move(slots));
    if (packet == nullptr) {
        return Status::NoMemory;
    }
    for (u32 i = 0; i < num_handles; ++i) {
        DEBUG_ASSERT(handles[i] && handles[i]->table_koid() == 0,
                     "a handle in a message belongs to no table");
        packet->m_handles[i] = ustl::move(handles[i]);
    }
    *out = ustl::Box<Self>::from_raw(packet);
    return Status::Ok;
}

auto MessagePacket::copy_data_to(user_copy::UserOutPtr<u8> const bytes) const -> Status {
    if (m_data.empty()) {
        return Status::Ok;
    }
    return bytes.copy_array_to_user(m_data.data(), num_bytes());
}
} // namespace ours::object
