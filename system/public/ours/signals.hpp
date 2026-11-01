/// Copyright (c) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef OURS_SIGNALS_HPP
#define OURS_SIGNALS_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// Object signals: the state of a kernel object that a program can wait on
/// through a handle (ours::object_wait_one).
///
/// The bit positions are ABI and never change. Positions 0..22 are per
/// object type: bit 3 is Signaled on an event and Terminated on a task, the
/// same bit read two ways. Named are the ones something in this tree raises
/// -- Readable, Writable and PeerClosed on a channel, Terminated on a
/// process and a thread, ThreadRunning on a thread, Idle on a virtual
/// interrupt, HandleClosed from object_wait_one -- plus Signaled,
/// ThreadSuspended and the user bits, which nothing raises yet; more are added
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

    /// Bit 3: SIGNALED on an event and TERMINATED on a task. A process raises
    /// it once it is dead, a thread once it has left its process; a job never
    /// does yet, and there is no event object.
    Signaled = 3_bit,
    Terminated = 3_bit,

    /// Bits 4 and 5, a thread's: RUNNING from its start until it stops or
    /// dies, SUSPENDED while a suspend holds it stopped. At most one of the
    /// two is up, neither before the thread starts, and neither once it is
    /// TERMINATED. Nothing raises SUSPENDED yet: nothing can suspend a thread.
    ThreadRunning = 4_bit,
    ThreadSuspended = 5_bit,

    /// Bit 6, a virtual interrupt's: IDLE, up while the object is owed no ack.
    /// It is up from the object's creation; interrupt_trigger drops it, and the
    /// ack of the last interrupt that leaves nothing more to deliver raises it
    /// again (the interrupt_wait that takes the next interrupt after the one it
    /// took, or interrupt_ack on a port); a destroy raises it for good. A
    /// level-triggered line's controller driver waits for it to unmask the pin
    /// it masked when it triggered. It is a level, not an event: a waiter that
    /// comes after the ack finds it up. A physical interrupt never raises it.
    Idle = 6_bit,

    /// Not a state of the object at all. object_wait_one reports it in its
    /// observed set when the handle it was waiting through leaves the table
    /// under it -- closed, or consumed by another call.
    HandleClosed = 23_bit,

    /// Bits 24..31 belong to user programs, and the kernel assigns them no
    /// meaning. Nothing can set them yet: there is no object_signal call to
    /// do it.
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
