/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "fake_kernel.hpp"

#include <algorithm>

#include <cstring>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace fake {
auto kernel() -> Kernel & {
    static auto THE_KERNEL = Kernel{};
    return THE_KERNEL;
}

auto Kernel::reset() -> void {
    *this = Kernel{};
}

auto Kernel::fail_next_wait_async(ours::Status const status) -> void {
    m_fail_next = status;
}

auto Kernel::is_open(ours::Uandle const handle) const -> bool {
    return m_objects.contains(handle);
}

auto Kernel::peer_closed(ours::Uandle const handle) const -> bool {
    if (!is_open(handle)) {
        return true;
    }
    auto const found = m_objects.find(handle);
    if (found == m_objects.end()) {
        return true;
    }
    return found->second.peer == ours::Uandle::Invalid;
}

auto Kernel::queued_messages(ours::Uandle const handle) const -> usize {
    return m_objects.at(handle).messages.size();
}

auto Kernel::closed() const -> std::vector<ours::Uandle> const & {
    return m_closed;
}

auto Kernel::debug_writes() const -> std::vector<std::string> const & {
    return m_debug_writes;
}

auto Kernel::make(Kind const kind) -> ours::Uandle {
    // The low two bits set, as every real handle value has them
    // (<ours/types.hpp>), so that no value here is Invalid.
    auto const handle = static_cast<ours::Uandle>((m_next << 2) | 3);
    ++m_next;
    m_objects.emplace(handle, Object{kind, ours::Uandle::Invalid, {}, {}});
    return handle;
}

auto Kernel::signals_of(Object const &object) const -> ours::Signals {
    auto signals = ours::Signals::None;
    if (!object.messages.empty()) {
        signals |= ours::Signals::Readable;
    }
    if (object.peer == ours::Uandle::Invalid) {
        signals |= ours::Signals::PeerClosed;
    } else {
        signals |= ours::Signals::Writable;
    }
    return signals;
}

auto Kernel::fire(ours::Uandle const handle) -> void {
    auto const observed = signals_of(m_objects.at(handle));
    // Each matching wait queues its packet in the order it was armed, and is
    // spent.
    auto const spent = [&](Armed const &wait) -> bool {
        if (wait.handle != handle || (wait.signals & observed) == ours::Signals::None) {
            return false;
        }
        auto packet = ours::PortPacket{};
        packet.key = wait.key;
        packet.type = ours::PacketType::SignalOne;
        packet.status = ours::Status::Ok;
        packet.signal = ours::PacketSignal{wait.signals, observed, 1, 0, 0};
        m_objects.at(wait.port).packets.push_back(packet);
        return true;
    };
    std::erase_if(m_armed, spent);
}

auto Kernel::channel_create(ours::Uandle *const out0, ours::Uandle *const out1) -> ours::Status {
    auto const first = make(Kind::Channel);
    auto const second = make(Kind::Channel);
    m_objects.at(first).peer = second;
    m_objects.at(second).peer = first;
    *out0 = first;
    *out1 = second;
    return ours::Status::Ok;
}

auto Kernel::channel_write(ours::Uandle const channel, void const *const bytes, u32 const num_bytes,
                           ours::Uandle const *const handles, u32 const num_handles)
    -> ours::Status {
    auto const found = m_objects.find(channel);
    if (found == m_objects.end() || found->second.kind != Kind::Channel) {
        return ours::Status::BadHandle;
    }
    auto message = Message{};
    message.bytes.resize(num_bytes);
    if (num_bytes != 0) {
        std::memcpy(message.bytes.data(), bytes, num_bytes);
    }
    message.handles.assign(handles, handles + num_handles);
    auto const peer = found->second.peer;
    if (peer == ours::Uandle::Invalid) {
        // The handles are consumed whatever the outcome.
        for (auto const handle: message.handles) {
            auto _ = handle_close(handle);
        }
        return ours::Status::PeerClosed;
    }
    m_objects.at(peer).messages.push_back(std::move(message));
    fire(peer);
    return ours::Status::Ok;
}

auto Kernel::channel_read(ours::Uandle const channel, void *const bytes,
                          ours::Uandle *const handles, u32 const num_bytes, u32 const num_handles,
                          u32 *const actual_bytes, u32 *const actual_handles) -> ours::Status {
    auto const found = m_objects.find(channel);
    if (found == m_objects.end() || found->second.kind != Kind::Channel) {
        return ours::Status::BadHandle;
    }
    auto &messages = found->second.messages;
    if (messages.empty()) {
        if (found->second.peer == ours::Uandle::Invalid) {
            return ours::Status::PeerClosed;
        }
        return ours::Status::ShouldWait;
    }
    auto const &message = messages.front();
    *actual_bytes = static_cast<u32>(message.bytes.size());
    *actual_handles = static_cast<u32>(message.handles.size());
    if (message.bytes.size() > num_bytes || message.handles.size() > num_handles) {
        return ours::Status::BufferTooSmall;
    }
    if (!message.bytes.empty()) {
        std::memcpy(bytes, message.bytes.data(), message.bytes.size());
    }
    std::ranges::copy(message.handles, handles);
    messages.pop_front();
    return ours::Status::Ok;
}

auto Kernel::handle_close(ours::Uandle const handle) -> ours::Status {
    m_closed.push_back(handle);
    if (handle == ours::Uandle::Invalid) {
        return ours::Status::Ok;
    }
    auto const found = m_objects.find(handle);
    if (found == m_objects.end()) {
        return ours::Status::BadHandle;
    }
    // Its armed waits end without a packet; a port takes the waits that
    // deliver to it.
    std::erase_if(m_armed, [&](Armed const &wait) -> bool {
        return wait.handle == handle || wait.port == handle;
    });
    auto const peer = found->second.peer;
    m_objects.erase(found);
    if (peer != ours::Uandle::Invalid) {
        m_objects.at(peer).peer = ours::Uandle::Invalid;
        fire(peer);
    }
    return ours::Status::Ok;
}

auto Kernel::port_create(ours::Uandle *const out) -> ours::Status {
    *out = make(Kind::Port);
    return ours::Status::Ok;
}

auto Kernel::port_wait(ours::Uandle const port, ours::Instant const deadline,
                       ours::PortPacket *const packet) -> ours::Status {
    auto const found = m_objects.find(port);
    if (found == m_objects.end() || found->second.kind != Kind::Port) {
        return ours::Status::BadHandle;
    }
    auto &packets = found->second.packets;
    if (packets.empty()) {
        // An INSTANT_INFINITE wait on an empty port would block for ever;
        // the fake answers BadState instead, which no real port_wait does,
        // so that a test sees it.
        if (deadline == 0) {
            return ours::Status::TimeOut;
        }
        return ours::Status::BadState;
    }
    *packet = packets.front();
    packets.pop_front();
    return ours::Status::Ok;
}

auto Kernel::object_wait_async(ours::Uandle const handle, ours::Uandle const port, u64 const key,
                               ours::Signals const signals, ours::WaitAsyncOptions const options)
    -> ours::Status {
    if (m_fail_next != ours::Status::Ok) {
        auto const status = m_fail_next;
        m_fail_next = ours::Status::Ok;
        return status;
    }
    auto const found_port = m_objects.find(port);
    if (found_port == m_objects.end() || found_port->second.kind != Kind::Port) {
        return ours::Status::BadHandle;
    }
    auto const found = m_objects.find(handle);
    if (found == m_objects.end() || found->second.kind != Kind::Channel) {
        return ours::Status::BadHandle;
    }
    if (options != ours::WaitAsyncOptions::None) {
        return ours::Status::InvArgs;
    }
    m_armed.push_back(Armed{handle, port, key, signals});
    fire(handle);
    return ours::Status::Ok;
}

auto Kernel::debug_write(char const *const buffer, usize const length) -> ours::Status {
    if (length > ours::MAX_DEBUG_WRITE) {
        return ours::Status::OutOfRange;
    }
    m_debug_writes.emplace_back(buffer, length);
    return ours::Status::Ok;
}
} // namespace fake

// The vDSO's entries, under their own symbols: each declaration in
// <ours/vdso.hpp> carries the asm label `ours_<name>`, and these definitions
// inherit it, so the library's calls bind here exactly as they bind to the
// vDSO on the target.
auto ours::channel_create(ChannelCreateOptions, Uandle *const out0, Uandle *const out1) -> Status {
    return fake::kernel().channel_create(out0, out1);
}

auto ours::channel_write(Uandle const channel, ChannelWriteOptions, void const *const bytes,
                         u32 const num_bytes, Uandle const *const handles, u32 const num_handles)
    -> Status {
    return fake::kernel().channel_write(channel, bytes, num_bytes, handles, num_handles);
}

auto ours::channel_read(Uandle const channel, ChannelReadOptions, void *const bytes,
                        Uandle *const handles, u32 const num_bytes, u32 const num_handles,
                        u32 *const actual_bytes, u32 *const actual_handles) -> Status {
    return fake::kernel().channel_read(channel, bytes, handles, num_bytes, num_handles,
                                       actual_bytes, actual_handles);
}

auto ours::handle_close(Uandle const handle) -> Status {
    return fake::kernel().handle_close(handle);
}

// The tree holds no file, so GetBackingMemory never reaches it; the server
// still takes its address.
auto ours::handle_duplicate(Uandle, Rights, Uandle *) -> Status {
    return Status::NotSupported;
}

auto ours::port_create(PortCreateOptions, Uandle *const out) -> Status {
    return fake::kernel().port_create(out);
}

auto ours::port_wait(Uandle const port, Instant const deadline, PortPacket *const packet)
    -> Status {
    return fake::kernel().port_wait(port, deadline, packet);
}

auto ours::object_wait_async(Uandle const handle, Uandle const port, u64 const key,
                             Signals const signals, WaitAsyncOptions const options) -> Status {
    return fake::kernel().object_wait_async(handle, port, key, signals, options);
}

auto ours::debug_write(char const *const buffer, usize const length) -> Status {
    return fake::kernel().debug_write(buffer, length);
}
