/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "fake_kernel.hpp"

#include <algorithm>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace fake {
auto kernel() -> Kernel & {
    static auto the_kernel = Kernel{};
    return the_kernel;
}

auto Kernel::reset() -> void {
    *this = Kernel{};
}

auto Kernel::make(Kind const kind) -> ours::Uandle {
    // The low two bits set, as every real handle value has them
    // (<ours/types.hpp>), so that no value here is Invalid.
    auto const handle = static_cast<ours::Uandle>((m_next << 2) | 3);
    ++m_next;
    m_objects.emplace(handle, Object{kind, 0, false, {}});
    return handle;
}

auto Kernel::make_port() -> ours::Uandle {
    return make(Kind::Port);
}

auto Kernel::make_object() -> ours::Uandle {
    return make(Kind::Channel);
}

auto Kernel::signals_of(Object const &object) const -> ours::Signals {
    auto signals = ours::Signals::None;
    if (object.messages > 0) {
        signals |= ours::Signals::Readable;
    }
    if (object.peer_closed) {
        signals |= ours::Signals::PeerClosed;
    } else {
        signals |= ours::Signals::Writable;
    }
    return signals;
}

auto Kernel::enqueue(ours::Uandle const port, u64 const key, ours::Signals const trigger,
                     ours::Signals const observed) -> void {
    auto packet = ours::PortPacket{};
    packet.key = key;
    packet.type = ours::PacketType::SignalOne;
    packet.status = ours::Status::Ok;
    packet.signal = ours::PacketSignal{trigger, observed, 1, 0, 0};
    m_objects.at(port).packets.push_back(packet);
}

auto Kernel::fire(ours::Uandle const handle) -> void {
    auto const observed = signals_of(m_objects.at(handle));
    // Each matching wait queues its packet in the order it was armed, and is
    // spent.
    auto const spent = [&](Armed const &wait) -> bool {
        if (wait.handle != handle || (wait.signals & observed) == ours::Signals::None) {
            return false;
        }
        enqueue(wait.port, wait.key, wait.signals, observed);
        return true;
    };
    std::erase_if(m_armed, spent);
}

auto Kernel::write(ours::Uandle const object, u32 const count) -> void {
    m_objects.at(object).messages += count;
    fire(object);
}

auto Kernel::read(ours::Uandle const object) -> bool {
    auto &found = m_objects.at(object);
    if (found.messages == 0) {
        return false;
    }
    --found.messages;
    return true;
}

auto Kernel::close_peer(ours::Uandle const object) -> void {
    m_objects.at(object).peer_closed = true;
    fire(object);
}

auto Kernel::queue(ours::Uandle const port, ours::PortPacket const &packet) -> void {
    m_objects.at(port).packets.push_back(packet);
}

auto Kernel::fail_next_wait_async(ours::Status const status) -> void {
    m_fail_next = status;
}

auto Kernel::is_open(ours::Uandle const handle) const -> bool {
    return m_objects.contains(handle);
}

auto Kernel::armed_on(ours::Uandle const handle) const -> usize {
    return static_cast<usize>(std::ranges::count_if(
        m_armed, [&](Armed const &wait) -> bool { return wait.handle == handle; }));
}

auto Kernel::queued_on(ours::Uandle const port) const -> usize {
    return m_objects.at(port).packets.size();
}

auto Kernel::closes() const -> std::vector<Close> const & {
    return m_closes;
}

auto Kernel::waits() const -> std::vector<WaitAsync> const & {
    return m_waits;
}

auto Kernel::deadlines() const -> std::vector<ours::Instant> const & {
    return m_deadlines;
}

auto Kernel::port_wait(ours::Uandle const port, ours::Instant const deadline,
                       ours::PortPacket *const packet) -> ours::Status {
    m_deadlines.push_back(deadline);
    auto const found = m_objects.find(port);
    if (found == m_objects.end()) {
        return ours::Status::BadHandle;
    }
    if (found->second.kind != Kind::Port) {
        return ours::Status::BadType;
    }
    if (deadline != 0 && deadline != ours::INSTANT_INFINITE) {
        return ours::Status::NotSupported;
    }
    auto &packets = found->second.packets;
    if (packets.empty()) {
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
    auto const answer = [&](ours::Status const status) -> ours::Status {
        m_waits.push_back(WaitAsync{handle, port, key, signals, status});
        return status;
    };
    if (m_fail_next != ours::Status::Ok) {
        auto const status = m_fail_next;
        m_fail_next = ours::Status::Ok;
        return answer(status);
    }
    // The kernel's order: the port, then the handle, then the options.
    auto const found_port = m_objects.find(port);
    if (found_port == m_objects.end()) {
        return answer(ours::Status::BadHandle);
    }
    if (found_port->second.kind != Kind::Port) {
        return answer(ours::Status::BadType);
    }
    auto const found = m_objects.find(handle);
    if (found == m_objects.end()) {
        return answer(ours::Status::BadHandle);
    }
    if (found->second.kind == Kind::Port) {
        // A port asserts no signals, so its handle has no Wait right.
        return answer(ours::Status::AccessDenied);
    }
    if (options != ours::WaitAsyncOptions::None) {
        return answer(ours::Status::InvArgs);
    }
    m_armed.push_back(Armed{handle, port, key, signals});
    fire(handle);
    return answer(ours::Status::Ok);
}

auto Kernel::handle_close(ours::Uandle const handle) -> ours::Status {
    auto const answer = [&](ours::Status const status) -> ours::Status {
        m_closes.push_back(Close{handle, status});
        return status;
    };
    if (handle == ours::Uandle::Invalid) {
        return answer(ours::Status::Ok);
    }
    auto const found = m_objects.find(handle);
    if (found == m_objects.end()) {
        return answer(ours::Status::BadHandle);
    }
    // Its armed waits end without a packet; packets already queued stay.
    // A port takes its queue with it, and the waits that deliver to it.
    std::erase_if(m_armed, [&](Armed const &wait) -> bool {
        return wait.handle == handle || wait.port == handle;
    });
    m_objects.erase(found);
    return answer(ours::Status::Ok);
}
} // namespace fake

// The vDSO's entries, under their own symbols: each declaration in
// <ours/vdso.hpp> carries the asm label `ours_<name>`, and these definitions
// inherit it, so the Loop's calls bind here exactly as they bind to the vDSO
// on the target.
auto ours::port_wait(Uandle const port, Instant const deadline, PortPacket *const packet)
    -> Status {
    return fake::kernel().port_wait(port, deadline, packet);
}

auto ours::object_wait_async(Uandle const handle, Uandle const port, u64 const key,
                             Signals const signals, WaitAsyncOptions const options) -> Status {
    return fake::kernel().object_wait_async(handle, port, key, signals, options);
}

auto ours::handle_close(Uandle const handle) -> Status {
    return fake::kernel().handle_close(handle);
}
