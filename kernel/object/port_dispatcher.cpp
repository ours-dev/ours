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
    /// Frees every packet on |packets|. Called with no lock held, on a list
    /// nothing else can reach any more.
    auto free_packets(QueuedPacket::List &packets) -> void {
        while (auto front = packets.front()) {
            auto &packet = *front;
            auto _ = packets.erase(packets.iterator_to(packet));
            auto owned = ustl::Box<QueuedPacket>::from_raw(&packet);
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
        // Freed after the guard below is dropped, at the end of the iteration.
        ustl::Box<QueuedPacket> taken;
        {
            Guard guard{m_port_lock};
            if (auto front = m_packets.front()) {
                auto &packet = *front;
                auto _ = m_packets.erase(m_packets.iterator_to(packet));

                // Every packet is a port_queue one until object_wait_async
                // queues packets of its own.
                DEBUG_ASSERT(packet.packet.type == PacketType::User);
                m_user_packets -= 1;

                if (m_packets.empty()) {
                    m_ready.unsignal();
                }
                *out = packet.packet;
                taken = ustl::Box<QueuedPacket>::from_raw(&packet);
            }
        }
        if (taken) {
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
} // namespace ours::object
