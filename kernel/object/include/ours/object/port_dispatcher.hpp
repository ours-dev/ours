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

class PortObserver;

/// One packet on a port's queue. Kernel-internal: what crosses the ABI is
/// |packet| alone.
///
/// A port_queue packet is allocated on its own, and |owner| is null. A
/// SignalOne packet lives inside the PortObserver that queued it, as in
/// Zircon, and |owner| is that observer: taking the packet off the queue
/// frees the observer, not the packet.
struct QueuedPacket {
    PortPacket packet;
    PortObserver *owner = nullptr;

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
    friend class PortObserver;

    /// Queues |observer|'s packet, reporting |observed|: the observer has
    /// matched, and the object it watched has already unlinked it. From here
    /// on the queue owns the observer.
    ///
    /// Returns the observer for the caller to free instead when the port has
    /// no handles left: no one could ever read the packet, and queuing it
    /// would keep the port alive through the observer's reference with no
    /// handle to break the cycle. The caller frees it once the port lock is
    /// dropped but still under the watched object's lock -- and, on some
    /// paths, a handle table's or a channel pair's lock -- so freeing it may
    /// run ~PortDispatcher there. That is why the destructor takes no lock and
    /// destroys no handle, and why a closing port must tear its observers
    /// down in on_zero_handles(), never in its destructor.
    [[nodiscard]]
    auto queue_observer_packet(PortObserver *observer, Signals observed) -> ustl::Box<PortObserver>;

    /// The observer's wait is cancelled, its handle gone. Returns the observer
    /// for the caller to free if it is still registered (m_object set), which
    /// it then no longer is; empty if someone else already took it.
    [[nodiscard]]
    auto maybe_reap(PortObserver *observer) -> ustl::Box<PortObserver>;

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
/// A one-shot, level-triggered wait on an object that delivers into a port
/// (object_wait_async). Zircon's PortObserver.
///
/// It is linked on the watched object's observer list, under that object's
/// lock, until one of three things ends the wait:
///   - the object asserts a signal it waits for: on_match() queues the
///     embedded packet, and the observer goes with it into the port's queue;
///   - the handle it was made through leaves its table: on_cancel() frees it
///     and nothing is queued;
///   - the port has lost its last handle by the time it matches: the packet
///     is dropped and the observer freed.
///
/// on_match() and on_cancel() cannot both run for one observer. Each is
/// called under the watched object's lock by the code that has just unlinked
/// the observer (or, for an immediate match, never linked it), so whichever
/// comes first is the only one that finds it.
///
/// |m_object| is for a race that object lock does not cover: a closing port
/// tearing down its armed observers, which runs under the port's lock rather
/// than the object's. It says whether the observer is still registered, is
/// read and written under the port's lock, and whoever sets it to null frees
/// the observer. Until a port tears its observers down, only the callbacks
/// set it, and it is always set when they look. Zircon's
/// PortObserver::dispatcher_.
class PortObserver final: public SignalObserver {
  public:
    PortObserver(ustl::Arc<PortDispatcher> port, Dispatcher *object, u64 key, Signals trigger);
    ~PortObserver() override = default;

    auto on_match(Signals signals) -> void override;
    auto on_cancel(Signals signals) -> void override;

  private:
    friend class PortDispatcher;

    QueuedPacket m_packet;
    ustl::Arc<PortDispatcher> const m_port;
    Dispatcher *m_object;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PORT_DISPATCHER_HPP
