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
/// waits for. Interrupt is what an interrupt bound to the port
/// (interrupt_bind) queues when it fires: one packet, until the interrupt is
/// acked (interrupt_ack). 2 to 6 are kept out of use: 2 belonged to a
/// repeating signal packet the ABI has retired, and 3 to 6 are the packets a
/// hypervisor's guests send, which this kernel does not have.
enum class PacketType : u32 {
    User = 0,
    SignalOne = 1,
    Interrupt = 7,
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

/// The payload of an Interrupt packet.
struct PacketInterrupt {
    /// When the interrupt arrived: the first one since the interrupt last
    /// sent a packet, as later ones only fold into it until the ack. For a
    /// virtual interrupt, the time interrupt_trigger was given.
    Instant timestamp;

    u64 reserved0;
    u64 reserved1;
    u64 reserved2;
};
static_assert(sizeof(PacketInterrupt) == 32);
static_assert(offsetof(PacketInterrupt, timestamp) == 0);

/// One packet, as port_queue takes it and port_wait hands it out.
///
/// Its 48 bytes cross the ABI, so the layout is pinned below. Further
/// payloads in the union -- guest, page-request -- arrive with the objects
/// that queue them, and do not change the size: every member is 32 bytes.
struct PortPacket {
    /// Chosen by whoever queued the packet, to tell its packets apart: the
    /// caller of port_queue, or the key given to object_wait_async or
    /// interrupt_bind.
    u64 key;

    PacketType type;

    /// Carried through unchanged from port_queue's caller; Ok for a
    /// SignalOne or an Interrupt packet.
    Status status;

    union {
        /// Opaque to the kernel: port_queue's caller decides what it means.
        u8 user[32];

        PacketSignal signal;

        PacketInterrupt interrupt;
    };
};
static_assert(sizeof(PortPacket) == 48);
static_assert(alignof(PortPacket) == 8);
static_assert(offsetof(PortPacket, key) == 0);
static_assert(offsetof(PortPacket, type) == 8);
static_assert(offsetof(PortPacket, status) == 12);
static_assert(offsetof(PortPacket, user) == 16);
static_assert(offsetof(PortPacket, signal) == 16);
static_assert(offsetof(PortPacket, interrupt) == 16);

/// port_create's options. One of the two, not bits: None, or
/// BindToInterrupt.
///
/// BindToInterrupt makes a port that interrupts can be bound to
/// (interrupt_bind). Such a port keeps their packets on a queue of its own,
/// and port_wait serves that queue before the other: an interrupt left
/// waiting is a device left waiting, often with its line masked.
enum class PortCreateOptions : u32 {
    None = 0,
    BindToInterrupt = 0x1,
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
