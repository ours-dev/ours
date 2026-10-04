/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PORT_DISPATCHER_HPP
#define OURS_OBJECT_PORT_DISPATCHER_HPP 1

#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/port.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>

#include <gktl/canary.hpp>
#include <ustl/box.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
/// Zircon ZX_DEFAULT_PORT_RIGHTS: Basic without Wait, because a port asserts
/// no signals of its own -- which is also what keeps a port from being
/// waited on through another port -- plus Read to dequeue and Write to queue.
constexpr auto RIGHTS_DEFAULT_PORT = (Rights::Basic & ~Rights::Wait) | Rights::Io;

/// One packet on a port's queue. Kernel-internal: what crosses the ABI is
/// |packet| alone.
///
/// A port_queue packet is allocated on its own. The packets object_wait_async
/// produces will live inside the observer that queues them, as in Zircon's
/// PortObserver, and carry an owner pointer to say so.
struct QueuedPacket {
    PortPacket packet;

    ustl::collections::intrusive::ListMemberHook<> m_queue_hook;
    USTL_DECLARE_HOOK_OPTION(QueuedPacket, m_queue_hook, QueueOption);
    USTL_DECLARE_LIST(QueuedPacket, List, QueueOption);
};

using PortDispatcherBase = SoloDispatcher<PortDispatcher, RIGHTS_DEFAULT_PORT>;

/// A queue of packets that threads wait on: the kernel half of an event
/// loop. Zircon's PortDispatcher (kernel/object/port_dispatcher.cc).
///
/// The objects:
///   - m_packets, the queue, and m_user_packets, how many of those came from
///     port_queue. Guarded by m_port_lock.
///   - m_ready, a manual-reset event the waiters sleep on. It is signalled
///     and unsignalled only under m_port_lock, so that "signalled" and "the
///     queue is not empty" are the same fact. Zircon posts a semaphore after
///     dropping its lock; with a manual-reset event that would leave it
///     signalled over an empty queue, and every waiter would spin on it.
///   - m_zero_handles, set once the last handle is gone. Nothing may queue
///     after that.
///
/// m_port_lock is its own Mutex, not the Dispatcher's m_lock: once
/// object_wait_async exists, an observer queues a packet while holding the
/// observed object's lock, and "object lock, then port lock" needs two locks
/// that lockdep can tell apart.
class PortDispatcher final: public PortDispatcherBase {
    using Base = PortDispatcherBase;
    using Self = PortDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Port);

    static auto create(KernelHandle<Self> *out, Rights *rights) -> Status;

    /// Queues |packet| as a User packet, with every other field as given --
    /// Zircon's QueueUser(), which keeps the caller's status and payload.
    ///
    /// ShouldWait when PORT_MAX_USER_PACKETS are already queued; BadHandle
    /// once the port has lost its last handle; NoMemory if the packet cannot
    /// be allocated.
    auto queue_user(PortPacket const &packet) -> Status;

    /// Takes the first packet into |out|.
    ///
    /// |deadline| is 0 to poll or INSTANT_INFINITE to wait for ever; any other
    /// value is NotSupported until a program can read a clock, as for
    /// wait_one(). An empty queue answers TimeOut to a poll. A wait sleeps
    /// until a packet arrives, interruptibly: a kill or a suspend ends it with
    /// the status the sleep returned. A waiter woken to find the queue emptied
    /// by another goes back to sleep.
    auto dequeue(Instant deadline, PortPacket *out) -> Status;

    auto on_zero_handles() -> void override;

    ~PortDispatcher() override;

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the canary.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("PortDispatcher");

    PortDispatcher() = default;

    sync::Mutex m_port_lock;
    QueuedPacket::List m_packets TA_GUARDED(m_port_lock);
    u32 m_user_packets TA_GUARDED(m_port_lock) = 0;
    bool m_zero_handles TA_GUARDED(m_port_lock) = false;
    sync::Event m_ready;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PORT_DISPATCHER_HPP
