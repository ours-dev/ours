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

namespace ours::object {
namespace {
    /// Frees |packet|, which is off every queue: the observer it lives in,
    /// for a SignalOne packet, or the packet itself, for a port_queue one.
    /// Called with no lock held, because freeing an observer drops its
    /// reference to the port.
    auto free_packet(QueuedPacket *const packet) -> void {
        if (packet->owner != nullptr) {
            auto owned = ustl::Box<PortObserver>::from_raw(packet->owner);
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

auto PortDispatcher::create(KernelHandle<Self> *const out, Rights *const rights) -> Status {
    auto port = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self());
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
/// lock of an object a PortObserver watched (Zircon's ~PortDispatcher only
/// asserts, for the same reason). Nothing is left to free by then:
/// on_zero_handles() emptied the queue, and nothing can queue after it.
PortDispatcher::~PortDispatcher() {
    DEBUG_ASSERT(m_packets.empty(), "port destroyed with packets queued");
    DEBUG_ASSERT(m_user_packets == 0, "port destroyed with {} user packets counted",
                 m_user_packets);
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

    Guard guard{m_port_lock};

    // Checked first, as in Zircon's QueuePacketLocked(): a port with no
    // handles has no one left to read it, so a packet queued now would never
    // be freed. Once observers hold the port, that would be a cycle.
    if (m_zero_handles) {
        return Status::BadHandle;
    }
    if (m_user_packets >= PORT_MAX_USER_PACKETS) {
        return Status::ShouldWait;
    }

    m_packets.push_back(*queued.into_raw());
    m_user_packets += 1;

    // Under the lock, so that m_ready is signalled exactly while the queue is
    // not empty.
    m_ready.signal();
    return Status::Ok;
}

auto PortDispatcher::dequeue(Instant const deadline, PortPacket *const out) -> Status {
    verify_canary();

    if (deadline != 0 && deadline != INSTANT_INFINITE) {
        return Status::NotSupported;
    }

    for (;;) {
        // Freed after the guard below is dropped.
        QueuedPacket *taken = nullptr;
        {
            Guard guard{m_port_lock};
            if (auto front = m_packets.front()) {
                auto &packet = *front;
                auto _ = m_packets.erase(m_packets.iterator_to(packet));
                if (packet.owner == nullptr) {
                    m_user_packets -= 1;
                }
                if (m_packets.empty()) {
                    m_ready.unsignal();
                }
                *out = packet.packet;
                taken = &packet;
            }
        }
        if (taken != nullptr) {
            free_packet(taken);
            return Status::Ok;
        }
        if (deadline == 0) {
            return Status::TimeOut;
        }

        // The event is signalled exactly while the queue is not empty, and a
        // packet queued between the check above and this wait leaves it
        // signalled, so the wait returns at once rather than missing it.
        // Woken, the loop looks again: another waiter may have taken the
        // packet first, and then the event is unsignalled and this sleeps
        // again. That is how more than one waiter shares a port -- a
        // duplicated handle in another process -- at the cost of waking
        // them all for one packet.
        if (auto const status = m_ready.wait(time::Deadline::infinite()); status != Status::Ok) {
            return status;
        }
    }
}

/// No packet can be read any more, so the queue goes, and nothing may queue
/// after this (m_zero_handles). The packets are freed outside the lock.
///
/// A thread sleeping in dequeue() is not woken, as in Zircon: its handle
/// was closed under it, by another thread of its process or by the process
/// ending, and it sleeps until it is killed. The event is unsignalled with the
/// queue emptied, so a waiter woken for some other reason goes back to sleep
/// rather than spinning.
auto PortDispatcher::on_zero_handles() -> void {
    QueuedPacket::List packets;
    {
        Guard guard{m_port_lock};
        m_zero_handles = true;
        packets.swap(m_packets);
        m_user_packets = 0;
        m_ready.unsignal();
    }
    free_packets(packets);
}

auto PortDispatcher::queue_observer_packet(PortObserver *const observer, Signals const observed)
    -> ustl::Box<PortObserver> {
    Guard guard{m_port_lock};

    // No longer registered: the object unlinked it before calling on_match().
    // Cleared here, under the port lock, whatever happens next, so that
    // nothing else takes it to free.
    observer->m_object = nullptr;

    // As for port_queue: a port with no handles left would never be read,
    // and the packet would keep it alive in a cycle (Zircon's
    // QueuePacketLocked()).
    if (m_zero_handles) {
        return ustl::Box<PortObserver>::from_raw(observer);
    }

    auto &packet = observer->m_packet;
    packet.packet.signal.observed = observed;
    packet.packet.signal.count = 1;
    m_packets.push_back(packet);
    m_ready.signal();
    return {};
}

auto PortDispatcher::maybe_reap(PortObserver *const observer) -> ustl::Box<PortObserver> {
    Guard guard{m_port_lock};
    if (observer->m_object == nullptr) {
        return {};
    }
    observer->m_object = nullptr;
    return ustl::Box<PortObserver>::from_raw(observer);
}

PortObserver::PortObserver(ustl::Arc<PortDispatcher> port, Dispatcher *const object, u64 const key,
                           Signals const trigger)
    : m_packet(), m_port(ustl::move(port)), m_object(object) {
    m_packet.packet.key = key;
    m_packet.packet.type = PacketType::SignalOne;
    m_packet.packet.status = Status::Ok;
    m_packet.packet.signal.trigger = trigger;
    m_packet.owner = this;
}

/// Under the watched object's lock, which has already unlinked this observer.
///
/// The port is reached through a local copy of the reference, not through
/// m_port, for two reasons. ustl::Rc passes const on: m_port is an
/// `Arc<PortDispatcher> const`, through which only const members can be
/// called. And the call below may free this observer -- dropping m_port,
/// perhaps the port's last reference -- while the port's own method has not
/// yet returned; the copy keeps the port alive until it has. Zircon's
/// OnMatch() takes port_ref for the second reason.
auto PortObserver::on_match(Signals const signals) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    // Destroyed before |port|: declared after it.
    auto release = port->queue_observer_packet(this, signals);
}

/// Under the watched object's lock, which has already unlinked this observer:
/// the handle the wait was made through has left its table. Nothing is
/// queued. Zircon's OnCancel(), through MaybeReap().
auto PortObserver::on_cancel(Signals const) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    auto release = port->maybe_reap(this);
}
} // namespace ours::object
