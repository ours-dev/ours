/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/syscall.hpp>
#include <ours/time/timer.hpp>

#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(DISPATCHER_PORT_CREATE_COUNT, "dispatcher.port.create");
// NOLINTNEXTLINE(readability-identifier-naming): as above.
GKTL_COUNTER(PORT_OBSERVER_TORN_DOWN, "port.observer.torn_down");
// NOLINTNEXTLINE(readability-identifier-naming): as above.
GKTL_COUNTER(PORT_OBSERVER_YIELDED, "port.observer.yielded_to_tear_down");

namespace ours::object {
namespace {
    /// Hands |packet|, which is off every queue, back to whoever frees it:
    /// its owner, the observer or kernel object it lives in, or the
    /// allocator, for a port_queue packet. Called with no lock held, because
    /// freeing an observer drops its reference to the port, and an owner may
    /// queue the packet again.
    auto free_packet(QueuedPacket *const packet) -> void {
        if (packet->owner != nullptr) {
            packet->owner->free(packet);
        } else {
            auto owned = ustl::Box<QueuedPacket>::from_raw(packet);
        }
    }

    /// Frees every packet on |packets|. Called with no lock held, on a list
    /// nothing else can reach any more.
    auto free_packets(QueuedPacket::List &packets) -> void {
        while (auto front = packets.front()) {
            auto &packet = *front;
            auto _ = packets.erase(packets.iterator_to(packet));
            free_packet(&packet);
        }
    }
} // namespace

auto PortDispatcher::create(PortCreateOptions const options, KernelHandle<Self> *const out,
                            Rights *const rights) -> Status {
    if (options != PortCreateOptions::None && options != PortCreateOptions::BindToInterrupt) {
        return Status::InvArgs;
    }

    auto port = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(options));
    if (!port) {
        return Status::NoMemory;
    }
    *out = KernelHandle<Self>(ustl::move(port));
    *rights = default_right();

    DISPATCHER_PORT_CREATE_COUNT.add(1);
    return Status::Ok;
}

/// Takes no lock and detaches nothing, only checks. The last reference can
/// drop anywhere, and once object_wait_async exists that includes under the
/// lock of an object a PortObserver watched. Nothing is left to free by then:
/// on_zero_handles() emptied the queue, and nothing can queue after it.
PortDispatcher::~PortDispatcher() {
    DEBUG_ASSERT(m_packets.empty(), "port destroyed with packets queued");
    DEBUG_ASSERT(m_user_packets == 0, "port destroyed with {} user packets counted",
                 m_user_packets);
    // Each armed observer holds a reference to this port, so none can be
    // left when the last reference goes; m_live_observers counts them and
    // the queued ones.
    DEBUG_ASSERT(m_armed.empty(), "port destroyed with observers armed");
    DEBUG_ASSERT(m_live_observers == 0, "port destroyed with {} observers counted",
                 m_live_observers);
    // Every interrupt bound here holds a reference to this port, and takes
    // its packet back when it is destroyed or unbound, which comes first.
    DEBUG_ASSERT(m_interrupt_packets.empty(), "port destroyed with interrupt packets queued");
    log::info("object: port (koid {}) destroyed", koid());
}

auto PortDispatcher::queue_user(PortPacket const &packet) -> Status {
    verify_canary();

    // Allocated before the lock is taken and, if the port refuses it, freed
    // after the lock is dropped: |queued| is declared before the guard, so it
    // is destroyed after it.
    auto queued = ustl::Box<QueuedPacket>::from_raw(new (mm::GAF_KERNEL) QueuedPacket{});
    if (!queued) {
        return Status::NoMemory;
    }
    queued->packet = packet;
    queued->packet.type = PacketType::User;

    {
        Guard guard{m_port_lock};

        // Checked first: a port with no handles has no one left to read it,
        // so a packet queued now would never be freed. Once observers hold the
        // port, that would be a cycle.
        if (!!(m_flags & Flags::ZeroHandles)) {
            return Status::BadHandle;
        }
        if (m_user_packets >= PORT_MAX_USER_PACKETS) {
            return Status::ShouldWait;
        }

        m_packets.push_back(*queued.into_raw());
        m_user_packets += 1;
    }

    // After the lock: the waiter this wakes takes the lock straight away, and
    // would only block on it here. Nothing is lost in the gap between the
    // push and the post, because every waiter takes a unit before it looks at
    // the queue: one that arrives in the gap finds no unit, sleeps, and this
    // post wakes it.
    m_sema.post();
    return Status::Ok;
}

auto PortDispatcher::dequeue(Instant const deadline, PortPacket *const out) -> Status {
    verify_canary();

    if (deadline != 0 && deadline != INSTANT_INFINITE) {
        return Status::NotSupported;
    }

    // A poll is a wait whose deadline has already passed: the semaphore
    // answers TimeOut at once when it has no unit, without queueing.
    auto wait_deadline = time::Deadline::infinite();
    if (deadline == 0) {
        wait_deadline = time::Deadline::at(TimePoint<>{});
    }

    for (;;) {
        // The unit first, the queues after. Each packet queued posted one
        // unit, so a waiter holding one finds a packet unless something took
        // packets off without taking their units: on_zero_handles(), which
        // empties the queue, or an interrupt taking its packet back. Then
        // the loop goes back to the semaphore, and a poll answers TimeOut
        // once the stale units are used up.
        if (auto const status = m_sema.wait(wait_deadline); status != Status::Ok) {
            return status;
        }

        // Interrupt packets first: an interrupt left waiting is a device left
        // waiting, its line perhaps masked until the ack. The interrupt lock
        // is let go of before the Mutex is taken; the two are never held
        // together.
        if (can_bind_to_interrupt() && dequeue_interrupt_packet(out)) {
            return Status::Ok;
        }

        // Freed after the guard below is dropped.
        QueuedPacket *taken = nullptr;
        {
            Guard guard{m_port_lock};
            if (auto front = m_packets.front()) {
                auto &packet = *front;
                auto _ = m_packets.erase(m_packets.iterator_to(packet));
                if (packet.origin == QueuedPacket::Origin::User) {
                    m_user_packets -= 1;
                } else if (packet.origin == QueuedPacket::Origin::Observer) {
                    m_live_observers -= 1;
                }
                *out = packet.packet;
                taken = &packet;
            }
        }
        if (taken != nullptr) {
            free_packet(taken);
            return Status::Ok;
        }
    }
}

auto PortDispatcher::queue(QueuedPacket *const packet) -> Status {
    verify_canary();
    DEBUG_ASSERT(packet->origin == QueuedPacket::Origin::Kernel && packet->owner != nullptr,
                 "only a packet a kernel object owns is queued this way");

    {
        Guard guard{m_port_lock};
        DEBUG_ASSERT(!packet->m_queue_hook.is_linked(), "a packet is queued once at a time");

        // As for port_queue: a port with no handles left would never be read.
        // The packet stays the caller's, since nothing else would ever free it.
        if (!!(m_flags & Flags::ZeroHandles)) {
            return Status::BadHandle;
        }
        m_packets.push_back(*packet);
    }

    // After the lock, as in queue_user(). From the unlock on, a waiter may
    // already have taken the packet and handed it back to its owner.
    m_sema.post();
    return Status::Ok;
}

/// Under the port lock, which every path that takes a packet off m_packets
/// holds while it does. Once the port has no handles, on_zero_handles() has
/// moved the queue to a list of its own, which it walks without the lock:
/// a packet linked there is not on m_packets, and erasing it from m_packets
/// would corrupt both. The flag, set in the same hold of the lock as the
/// move, says which: then the tear-down has the packet and frees it.
auto PortDispatcher::cancel_queued(QueuedPacket *const packet) -> bool {
    verify_canary();

    Guard guard{m_port_lock};
    if (!!(m_flags & Flags::ZeroHandles) || !packet->m_queue_hook.is_linked()) {
        return false;
    }
    auto _ = m_packets.erase(m_packets.iterator_to(*packet));
    return true;
}

/// No packet can be read any more, so the queue goes, nothing may queue after
/// this (Flags::ZeroHandles), and every armed observer is taken off its object
/// and freed. The packets are freed outside the lock.
///
/// A thread sleeping in dequeue() is not woken: its handle was closed under
/// it, by another thread of its process or by the process ending, and it
/// sleeps until it is killed. The units the emptied packets posted stay on the
/// semaphore; a waiter that takes one finds the queue empty and goes back to
/// it, so the stale units are used up one look each rather than spinning.
///
/// The armed observers are why this cannot wait for the destructor: each
/// holds a reference to the port, so a closed port with waits armed would
/// live until every one of them fired or was cancelled -- for ever, for a
/// wait on something that never happens.
auto PortDispatcher::on_zero_handles() -> void {
    QueuedPacket::List packets;
    {
        Guard guard{m_port_lock};
        m_flags |= Flags::ZeroHandles;
        packets.swap(m_packets);
        m_user_packets = 0;
        for (auto const &packet: packets) {
            if (packet.origin == QueuedPacket::Origin::Observer) {
                m_live_observers -= 1;
            }
        }
    }
    free_packets(packets);

    // One observer at a time, each in three steps:
    //   1. Under the port lock, take it off m_armed and claim it by setting
    //      m_object to null, keeping a reference to the object so it outlives
    //      step 2. A callback that runs on it from now on finds m_object null
    //      and leaves it alone.
    //   2. With the port lock dropped, take it off the object. The object's
    //      lock comes after the port's nowhere ("object, then port" is the
    //      only order), so it is never taken while the port lock is held.
    //      The object may have unlinked it already, to call a callback that
    //      is waiting for the port lock or has just left it; detach then
    //      finds nothing, and it has had to wait for that callback to finish
    //      with the observer, because the callback runs under the object's
    //      lock.
    //   3. Free it: nothing else can reach it any more.
    for (;;) {
        PortObserver *observer = nullptr;
        ustl::Arc<Dispatcher> object;
        {
            Guard guard{m_port_lock};
            auto front = m_armed.front();
            if (!front) {
                break;
            }
            observer = &*front;
            auto _ = m_armed.erase(m_armed.iterator_to(*observer));
            m_live_observers -= 1;

            // Alive: the observer is still registered (m_object set, under
            // this lock), so it is linked on the object, or a callback on it
            // is running under the object's lock, and either way the handle
            // it was attached through has not finished dying.
            DEBUG_ASSERT(observer->m_object != nullptr);
            object = ustl::Arc<Dispatcher>(observer->m_object);
            observer->m_object = nullptr;
        }
        auto ignored = Signals::None;
        auto _ = object->detach_observer(observer, &ignored);
        auto torn_down = ustl::Box<PortObserver>::from_raw(observer);
        PORT_OBSERVER_TORN_DOWN.add(1);
    }
}

auto PortDispatcher::dequeue_interrupt_packet(PortPacket *const out) -> bool {
    Guard<sync::SpinLock, IrqSave> const guard{m_interrupt_lock};
    auto front = m_interrupt_packets.front();
    if (!front) {
        return false;
    }
    auto &packet = *front;
    auto _ = m_interrupt_packets.erase(m_interrupt_packets.iterator_to(packet));

    // Read under the lock: once it is dropped the interrupt may queue the
    // packet again, or take it back and be freed.
    *out = PortPacket{};
    out->key = packet.key;
    out->type = PacketType::Interrupt;
    out->status = Status::Ok;
    out->interrupt.timestamp = packet.timestamp;
    return true;
}

/// The post comes after the lock, as in queue_user(), and touches nothing of
/// the packet: a waiter may take it off the queue at once. The port stays
/// alive across the post because the interrupt holds a reference to it.
auto PortDispatcher::queue_interrupt_packet(PortInterruptPacket *const packet,
                                            Instant const timestamp) -> bool {
    {
        Guard<sync::SpinLock, IrqSave> const guard{m_interrupt_lock};
        if (packet->m_queue_hook.is_linked()) {
            return false;
        }
        packet->timestamp = timestamp;
        m_interrupt_packets.push_back(*packet);
    }
    m_sema.post();
    return true;
}

auto PortDispatcher::remove_interrupt_packet(PortInterruptPacket *const packet) -> bool {
    Guard<sync::SpinLock, IrqSave> const guard{m_interrupt_lock};
    if (!packet->m_queue_hook.is_linked()) {
        return false;
    }
    auto _ = m_interrupt_packets.erase(m_interrupt_packets.iterator_to(*packet));
    return true;
}

auto PortDispatcher::tear_down_counts() -> TearDownCounts {
    return {
        .torn_down = PORT_OBSERVER_TORN_DOWN.sum_across_all_cpus(),
        .yielded = PORT_OBSERVER_YIELDED.sum_across_all_cpus(),
    };
}

auto PortDispatcher::register_observer(PortObserver *const observer) -> Status {
    Guard guard{m_port_lock};

    // The caller holds a handle to this port in its table, under that table's
    // lock, so the port cannot have lost its last handle. Checked anyway: a
    // registration after the tear-down would never be undone.
    if (!!(m_flags & Flags::ZeroHandles)) {
        return Status::BadHandle;
    }
    if (m_live_observers >= PORT_MAX_OBSERVERS) {
        return Status::NoResource;
    }
    m_armed.push_back(*observer);
    m_live_observers += 1;
    return Status::Ok;
}

auto PortDispatcher::queue_observer_packet(PortObserver *const observer, Signals const observed)
    -> ustl::Box<PortObserver> {
    {
        Guard guard{m_port_lock};

        // The port's tear-down got here first and owns it: it frees the
        // observer once it has taken it off the object, which waits for this
        // callback to finish. Queue nothing and free nothing.
        if (observer->m_object == nullptr) {
            PORT_OBSERVER_YIELDED.add(1);
            return {};
        }

        // No longer armed: the object unlinked it before calling on_match().
        observer->m_object = nullptr;
        auto _ = m_armed.erase(m_armed.iterator_to(*observer));

        // As for port_queue: a port with no handles left would never be read,
        // and the packet would keep it alive in a cycle.
        if (!!(m_flags & Flags::ZeroHandles)) {
            m_live_observers -= 1;
            return ustl::Box<PortObserver>::from_raw(observer);
        }

        auto &packet = observer->m_packet;
        packet.packet.signal.observed = observed;
        packet.packet.signal.count = 1;
        m_packets.push_back(packet);
    }

    // After the lock, as in queue_user(). From the unlock on, a waiter may
    // already have taken the packet and freed |observer| with it, so nothing
    // here touches the observer again. The port stays alive: on_match()
    // holds a reference to it for the length of this call.
    m_sema.post();
    return {};
}

auto PortDispatcher::maybe_reap(PortObserver *const observer) -> ustl::Box<PortObserver> {
    Guard guard{m_port_lock};
    // The port's tear-down got here first; see queue_observer_packet().
    if (observer->m_object == nullptr) {
        PORT_OBSERVER_YIELDED.add(1);
        return {};
    }
    observer->m_object = nullptr;
    auto _ = m_armed.erase(m_armed.iterator_to(*observer));
    m_live_observers -= 1;
    return ustl::Box<PortObserver>::from_raw(observer);
}

PortObserver::PortObserver(ustl::Arc<PortDispatcher> port, Dispatcher *const object, u64 const key,
                           Signals const trigger)
    : m_packet(), m_port(ustl::move(port)), m_object(object) {
    m_packet.packet.key = key;
    m_packet.packet.type = PacketType::SignalOne;
    m_packet.packet.status = Status::Ok;
    m_packet.packet.signal.trigger = trigger;
    m_packet.origin = QueuedPacket::Origin::Observer;
    m_packet.owner = this;
}

PortObserver::~PortObserver() = default;

/// With no lock of the port's held. Freeing the observer drops its reference
/// to the port, perhaps the last.
auto PortObserver::free(QueuedPacket *const packet) -> void {
    DEBUG_ASSERT(packet == &m_packet, "an observer frees only its own packet");
    auto owned = ustl::Box<PortObserver>::from_raw(this);
}

/// Under the watched object's lock, which has already unlinked this observer.
///
/// The port is reached through a local copy of the reference, not through
/// m_port, for two reasons. ustl::Rc passes const on: m_port is an
/// `Arc<PortDispatcher> const`, through which only const members can be
/// called. And the call below may free this observer -- dropping m_port,
/// perhaps the port's last reference -- while the port's own method has not
/// yet returned; the copy keeps the port alive until it has.
auto PortObserver::on_match(Signals const signals) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    // Destroyed before |port|: declared after it.
    auto release = port->queue_observer_packet(this, signals);
}

/// Under the watched object's lock, which has already unlinked this observer:
/// the handle the wait was made through has left its table. Nothing is
/// queued: maybe_reap() hands the observer back and it is freed here, unless
/// the port's tear-down has already claimed it.
auto PortObserver::on_cancel(Signals const) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    auto release = port->maybe_reap(this);
}
} // namespace ours::object
