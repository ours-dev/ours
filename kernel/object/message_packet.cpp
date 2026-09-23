/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/gaf.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/syscall.hpp>

#include <heap/kmalloc.hpp>
#include <heap/new.hpp>

namespace ours::object {
MessagePacket::MessagePacket(u8 *const data, u32 const num_bytes)
    : m_data(data), m_num_bytes(num_bytes), m_queue_hook() {}

MessagePacket::~MessagePacket() {
    if (m_data != nullptr) {
        heap::kfree(m_data);
    }
}

auto MessagePacket::create(user_copy::UserInPtr<u8 const> const bytes, u32 const num_bytes,
                           ustl::Box<Self> *const out) -> Status {
    if (num_bytes > CHANNEL_MAX_MSG_BYTES) {
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

    auto *const packet = new (mm::GAF_KERNEL) Self(data, num_bytes);
    if (packet == nullptr) {
        if (data != nullptr) {
            heap::kfree(data);
        }
        return Status::NoMemory;
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
