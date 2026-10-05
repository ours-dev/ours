/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_PORT_HPP
#define OURS_PORT_HPP 1

#include <ours/types.hpp>

#include <cstddef>

namespace ours {
/// What a port packet reports. The values are ABI and never change.
///
/// port_queue always delivers User, whatever type its caller wrote: the
/// other types are the kernel's to produce. SignalOne is what an
/// object_wait_async observer queues when the object asserts a signal it
/// waits for.
enum class PacketType : u32 {
    User = 0,
    SignalOne = 1,
};

/// The payload of a SignalOne packet.
struct PacketSignal {
    /// The signals the wait asked for.
    Signals trigger;

    /// The signals the object asserted when the packet was queued.
    Signals observed;

    /// Always 1: a one-shot wait matches once.
    u64 count;

    /// Zero until a WaitAsyncOptions::Timestamp exists.
    Instant timestamp;

    u64 reserved;
};
static_assert(sizeof(PacketSignal) == 32);
static_assert(offsetof(PacketSignal, trigger) == 0);
static_assert(offsetof(PacketSignal, observed) == 4);
static_assert(offsetof(PacketSignal, count) == 8);
static_assert(offsetof(PacketSignal, timestamp) == 16);
static_assert(offsetof(PacketSignal, reserved) == 24);

/// One packet, as port_queue takes it and port_wait hands it out.
///
/// Its 48 bytes cross the ABI, so the layout is pinned below. Further
/// payloads in the union -- interrupt, guest, page-request -- arrive with the
/// objects that queue them, and do not change the size: every member is 32
/// bytes.
struct PortPacket {
    /// Chosen by whoever queued the packet, to tell its packets apart: the
    /// caller of port_queue, or the key given to object_wait_async.
    u64 key;

    PacketType type;

    /// Carried through unchanged from port_queue's caller; Ok for a
    /// SignalOne packet.
    Status status;

    union {
        /// Opaque to the kernel: port_queue's caller decides what it means.
        u8 user[32];

        PacketSignal signal;
    };
};
static_assert(sizeof(PortPacket) == 48);
static_assert(alignof(PortPacket) == 8);
static_assert(offsetof(PortPacket, key) == 0);
static_assert(offsetof(PortPacket, type) == 8);
static_assert(offsetof(PortPacket, status) == 12);
static_assert(offsetof(PortPacket, user) == 16);
static_assert(offsetof(PortPacket, signal) == 16);

/// port_create's options. None is the only one; bit 0 is kept for an option
/// that lets the port take interrupt packets, which comes with binding an
/// interrupt object to a port (interrupt_bind).
enum class PortCreateOptions : u32 {
    None = 0,
};

/// object_wait_async's options. None is the only one for now: a wait is
/// one-shot and level-triggered, and its packet carries no timestamp.
/// Bits 0 and 1 are kept for a timestamped packet and an edge-triggered
/// wait, which come in P2.
enum class WaitAsyncOptions : u32 {
    None = 0,
};

/// The most object_wait_async observers one port has live: armed, or matched
/// with their packet still queued. A wait past it answers NoResource.
///
/// Queued packets count, not only armed observers: that bounds what a port
/// can hold for a reader that never reads. Otherwise a program could arm a
/// wait on a signal already asserted, have it queue at once, and arm again
/// without end. A program past the limit gets a status, not an exception.
inline constexpr u32 PORT_MAX_OBSERVERS = 1024;

/// The most user packets (port_queue) one port holds unread. A port_queue
/// that would exceed it answers ShouldWait: the reader has fallen behind,
/// and the caller should wait for it rather than have the kernel buffer
/// without bound.
///
/// Only port_queue's packets count, none the kernel queues. A caller past
/// the limit gets a status it can back off on, not an exception.
inline constexpr u32 PORT_MAX_USER_PACKETS = 256;
} // namespace ours

#endif // #ifndef OURS_PORT_HPP
