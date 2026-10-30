/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOOP_TEST_FAKE_KERNEL_HPP
#define LOOP_TEST_FAKE_KERNEL_HPP 1

#include <deque>
#include <map>
#include <vector>

#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

/// The three system calls the Loop makes -- port_wait, object_wait_async and
/// handle_close -- answered on the host by a model small enough to read in
/// one sitting. fake_kernel.cpp defines ours::port_wait() and the other two
/// under the vDSO's own symbol names (<ours/vdso.hpp>), so the Loop's source
/// is compiled unchanged and simply links against these instead. A call the
/// Loop is not meant to make has no definition here, and the test fails to
/// link.
///
/// The model keeps the kernel's rules that the Loop depends on:
///   - a wait is one-shot and level-triggered: armed on an object that
///     already asserts one of its signals, its packet is queued at once, at
///     the back of the port's queue; otherwise it is queued when the object
///     first asserts one, and the wait is spent;
///   - closing a handle ends its armed waits without a packet, but a packet
///     already queued stays (check_wait_async() in system/app/shell checks
///     the same on the real kernel);
///   - a port hands its packets out in the order they were queued.
///
/// Objects are channel ends as far as signals go: Writable while the peer
/// is there, Readable while messages are queued, PeerClosed once the peer is
/// gone. A test moves them with write(), read() and close_peer().
namespace fake {
/// One handle_close call: what it was given and what it answered.
struct Close {
    ours::Uandle handle;
    ours::Status status;
};

/// One object_wait_async call.
struct WaitAsync {
    ours::Uandle handle;
    ours::Uandle port;
    u64 key;
    ours::Signals signals;
    ours::Status status;
};

class Kernel final {
  public:
    /// Forgets every object, packet, wait and record.
    auto reset() -> void;

    /// A new port, or a new channel end with nothing queued.
    auto make_port() -> ours::Uandle;
    auto make_object() -> ours::Uandle;

    /// Queues |count| messages on |object|: Readable.
    auto write(ours::Uandle object, u32 count) -> void;

    /// Takes one message off |object|, as a Waiter's channel_read would;
    /// false when none was queued.
    auto read(ours::Uandle object) -> bool;

    /// The peer goes: PeerClosed, and no longer Writable.
    auto close_peer(ours::Uandle object) -> void;

    /// Queues |packet| on |port| as it is, as a holder of a duplicate of the
    /// port could with port_queue or a wait of its own.
    auto queue(ours::Uandle port, ours::PortPacket const &packet) -> void;

    /// The next object_wait_async answers |status| and arms nothing.
    auto fail_next_wait_async(ours::Status status) -> void;

    /// Whether |handle| names an open object.
    [[nodiscard]]
    auto is_open(ours::Uandle handle) const -> bool;

    /// How many waits are armed on |handle|, and how many packets are queued
    /// on |port|.
    [[nodiscard]]
    auto armed_on(ours::Uandle handle) const -> usize;
    [[nodiscard]]
    auto queued_on(ours::Uandle port) const -> usize;

    /// Every call made so far, oldest first.
    [[nodiscard]]
    auto closes() const -> std::vector<Close> const &;
    [[nodiscard]]
    auto waits() const -> std::vector<WaitAsync> const &;
    [[nodiscard]]
    auto deadlines() const -> std::vector<ours::Instant> const &;

    /// The system calls, as fake_kernel.cpp's definitions forward them. An
    /// INSTANT_INFINITE port_wait on an empty port would block forever; the
    /// fake answers BadState instead, which no real port_wait does, so that a
    /// test sees it.
    auto port_wait(ours::Uandle port, ours::Instant deadline, ours::PortPacket *packet)
        -> ours::Status;
    auto object_wait_async(ours::Uandle handle, ours::Uandle port, u64 key, ours::Signals signals,
                           ours::WaitAsyncOptions options) -> ours::Status;
    auto handle_close(ours::Uandle handle) -> ours::Status;

  private:
    struct Armed {
        ours::Uandle handle;
        ours::Uandle port;
        u64 key;
        ours::Signals signals;
    };

    enum class Kind : u8 {
        Port,
        Channel,
    };

    struct Object {
        Kind kind;
        u32 messages;
        bool peer_closed;
        std::deque<ours::PortPacket> packets;
    };

    auto make(Kind kind) -> ours::Uandle;
    [[nodiscard]]
    auto signals_of(Object const &object) const -> ours::Signals;

    /// Queues a packet for every armed wait on |handle| whose signals the
    /// object now asserts, and drops those waits.
    auto fire(ours::Uandle handle) -> void;

    auto enqueue(ours::Uandle port, u64 key, ours::Signals trigger, ours::Signals observed) -> void;

    u32 m_next = 1;
    std::map<ours::Uandle, Object> m_objects;
    std::vector<Armed> m_armed;
    ours::Status m_fail_next = ours::Status::Ok;
    std::vector<Close> m_closes;
    std::vector<WaitAsync> m_waits;
    std::vector<ours::Instant> m_deadlines;
};

/// The one model the fake system calls reach.
auto kernel() -> Kernel &;
} // namespace fake

#endif // #ifndef LOOP_TEST_FAKE_KERNEL_HPP
