/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef TESTING_FAKE_KERNEL_HPP
#define TESTING_FAKE_KERNEL_HPP 1

#include <deque>
#include <map>
#include <string>
#include <vector>

#include <ours/debug.hpp>
#include <ours/object.hpp>
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

/// The system calls a Loop and the servers on it make (ioservice::LoopServer,
/// ioservice::Outgoing, server::ProtocolServer), answered on the host by a model of
/// ports and channels small enough to read in one sitting.
/// fake_kernel.cpp defines them under the vDSO's own symbol names
/// (<ours/vdso.hpp>), so the library's source is compiled unchanged and links
/// against these instead: channel_create, channel_read, channel_write,
/// handle_close, handle_duplicate, object_wait_async, port_create,
/// port_wait and debug_write. A call the library is not meant to make has no definition
/// here, and the test fails to link.
///
/// The model keeps the kernel's rules the library depends on:
///   - a channel end is Readable while messages are queued on it, and
///     PeerClosed once its peer is closed, Writable before;
///   - a write queues on the peer, and fails with PeerClosed once the peer
///     is gone, consuming the handles either way;
///   - a wait is one-shot and level-triggered: armed on an object that
///     already asserts one of its signals, its packet is queued at once;
///     otherwise when the object first asserts one;
///   - closing a handle ends its armed waits without a packet, and closes
///     the channel for its peer;
///   - a port hands its packets out in the order they were queued;
///   - a wait a test has made fail (fail_next_wait_async()) answers the
///     status it was given and arms nothing;
///   - a debug_write longer than MAX_DEBUG_WRITE answers OutOfRange and
///     writes nothing, as the kernel does, so that a library that does not
///     split or cut its line shows as a line that is missing.
///
/// What it leaves out: rights, and moving handles between tables. A handle
/// written into a message keeps its value and stays open in the model, and
/// reading the message hands the same value back, so a test can follow a
/// channel end through the outgoing directory by its value.
namespace fake {
class Kernel final {
  public:
    /// Forgets every object, packet, wait and record.
    auto reset() -> void;

    /// The next object_wait_async answers |status| and arms nothing, as the
    /// kernel does when it has no memory for the wait. A Loop's re-arm is an
    /// object_wait_async, so this is how a test makes one fail.
    auto fail_next_wait_async(ours::Status status) -> void;

    /// Whether |handle| names an open object.
    [[nodiscard]]
    auto is_open(ours::Uandle handle) const -> bool;

    /// How many messages are queued on the channel end |handle|.
    [[nodiscard]]
    auto queued_messages(ours::Uandle handle) const -> usize;

    /// Every handle_close call's handle, oldest first.
    [[nodiscard]]
    auto closed() const -> std::vector<ours::Uandle> const &;

    /// What each debug_write call that was accepted wrote, oldest first.
    [[nodiscard]]
    auto debug_writes() const -> std::vector<std::string> const &;

    auto channel_create(ours::Uandle *out0, ours::Uandle *out1) -> ours::Status;
    auto channel_write(ours::Uandle channel, void const *bytes, u32 num_bytes,
                       ours::Uandle const *handles, u32 num_handles) -> ours::Status;
    auto channel_read(ours::Uandle channel, void *bytes, ours::Uandle *handles, u32 num_bytes,
                      u32 num_handles, u32 *actual_bytes, u32 *actual_handles) -> ours::Status;
    auto handle_close(ours::Uandle handle) -> ours::Status;
    auto port_create(ours::Uandle *out) -> ours::Status;
    auto port_wait(ours::Uandle port, ours::Instant deadline, ours::PortPacket *packet)
        -> ours::Status;
    auto object_wait_async(ours::Uandle handle, ours::Uandle port, u64 key, ours::Signals signals,
                           ours::WaitAsyncOptions options) -> ours::Status;
    auto debug_write(char const *buffer, usize length) -> ours::Status;

  private:
    struct Message {
        std::vector<u8> bytes;
        std::vector<ours::Uandle> handles;
    };

    enum class Kind : u8 {
        Port,
        Channel,
    };

    struct Object {
        Kind kind;
        /// A channel end's peer; Invalid once it is closed.
        ours::Uandle peer;
        std::deque<Message> messages;
        std::deque<ours::PortPacket> packets;
    };

    struct Armed {
        ours::Uandle handle;
        ours::Uandle port;
        u64 key;
        ours::Signals signals;
    };

    auto make(Kind kind) -> ours::Uandle;

    [[nodiscard]]
    auto signals_of(Object const &object) const -> ours::Signals;

    /// Queues a packet for every armed wait on |handle| whose signals the
    /// object now asserts, and drops those waits.
    auto fire(ours::Uandle handle) -> void;

    u32 m_next = 1;
    std::map<ours::Uandle, Object> m_objects;
    std::vector<Armed> m_armed;
    std::vector<ours::Uandle> m_closed;
    std::vector<std::string> m_debug_writes;
    ours::Status m_fail_next = ours::Status::Ok;
};

/// The one model the fake system calls reach.
auto kernel() -> Kernel &;
} // namespace fake

#endif // #ifndef TESTING_FAKE_KERNEL_HPP
