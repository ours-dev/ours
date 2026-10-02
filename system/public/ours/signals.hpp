/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_SIGNALS_HPP
#define OURS_SIGNALS_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// Object signals: the state of a kernel object that a program can wait on
/// through a handle (ours_object_wait_one).
///
/// The bit positions are Zircon's, deliberately -- ZX_OBJECT_READABLE and
/// friends in zircon/types.h -- so that anything written against Zircon's
/// documentation of what a signal means lands on the same bit here. Positions
/// 0..22 are per object type: bit 3 is SIGNALED on an event and TERMINATED on
/// a task, the same bit read two ways, exactly as in Zircon. Named are the
/// ones something in this tree raises -- Readable, Writable and PeerClosed on
/// a channel, Terminated on a process, HandleClosed from object_wait_one --
/// plus Signaled and the user bits, which nothing raises yet; more are added
/// with the objects that raise them.
///
/// This enum used to hold Suspend and Kill. Those are orders the kernel gives
/// its own threads, not observable object state, and they now live in
/// task::ThreadSignals. Kill sat on bit 1, which is WRITABLE.
enum class Signals : u32 {
    None = 0,

    /// Bits 0..2, shared by every object that has a peer or a queue.
    Readable = 0_bit,
    Writable = 1_bit,
    PeerClosed = 2_bit,

    /// Bit 3: the one per-type bit so far, SIGNALED on an event and TERMINATED
    /// on a task. Only a process raises it yet, once it is dead; a thread or a
    /// job never does, and there is no event object.
    Signaled = 3_bit,
    Terminated = 3_bit,

    /// Not a state of the object at all. object_wait_one reports it in its
    /// observed set when the handle it was waiting through leaves the table
    /// under it -- closed, or consumed by another call (Zircon
    /// ZX_SIGNAL_HANDLE_CLOSED).
    HandleClosed = 23_bit,

    /// Bits 24..31 belong to user programs, and the kernel assigns them no
    /// meaning. Nothing can set them yet: object_signal, Zircon's call for
    /// it, does not exist here.
    User0 = 24_bit,
    User1 = 25_bit,
    User2 = 26_bit,
    User3 = 27_bit,
    User4 = 28_bit,
    User5 = 29_bit,
    User6 = 30_bit,
    User7 = 31_bit,

    UserAll = User0 | User1 | User2 | User3 | User4 | User5 | User6 | User7,
};
USTL_ENABLE_ENUM_BITMASK(Signals);
} // namespace ours

#endif // #ifndef OURS_SIGNALS_HPP
