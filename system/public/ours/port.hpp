/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_PORT_HPP
#define OURS_PORT_HPP 1

#include <ours/types.hpp>

#include <cstddef>

namespace ours {
/// What a port packet reports. The values are Zircon's ZX_PKT_TYPE_*
/// (zircon/syscalls/port.h).
///
/// port_queue always delivers User, whatever type its caller wrote: the
/// other types are the kernel's to produce. SignalOne is what an
/// object_wait_async observer queues when the object asserts a signal it
/// waits for.
enum class PacketType : u32 {
    User = 0,
    SignalOne = 1,
};

/// The payload of a SignalOne packet. Zircon's zx_packet_signal_t.
struct PacketSignal {
    /// The signals the wait asked for.
    Signals trigger;

    /// The signals the object asserted when the packet was queued.
    Signals observed;

    /// Always 1 for a one-shot wait, as in Zircon.
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
/// Zircon's zx_port_packet_t, field for field, so that its 48 bytes cross
/// the ABI with the same layout. Its union also has guest, interrupt and
/// page-request payloads; those arrive with the objects that queue them,
/// and do not change the size: every member is 32 bytes.
struct PortPacket {
    /// Chosen by whoever queued the packet, to tell its packets apart: the
    /// caller of port_queue, or the key given to object_wait_async.
    u64 key;

    PacketType type;

    /// Carried through unchanged from port_queue's caller, as in Zircon; Ok
    /// for a SignalOne packet.
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

/// port_create's options. None is the only one; Zircon's ZX_PORT_BIND_TO_INTERRUPT
/// comes with interrupt objects.
enum class PortCreateOptions : u32 {
    None = 0,
};

/// The most user packets (port_queue) one port holds unread. A port_queue
/// that would exceed it answers ShouldWait: the reader has fallen behind,
/// and the caller should wait for it rather than have the kernel buffer
/// without bound.
///
/// Zircon counts the same packets, port_queue's and no others
/// (kMaxAllocatedPacketCountPerPort in object/port_dispatcher.cc, checked in
/// QueuePacketLocked()), but allows 4096, and a caller past the limit gets a
/// policy exception rather than a status. Here it is a status the caller can
/// back off on.
inline constexpr u32 PORT_MAX_USER_PACKETS = 256;
} // namespace ours

#endif // #ifndef OURS_PORT_HPP
