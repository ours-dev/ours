/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/pager_proxy.hpp>
#include <ours/time/timer.hpp>

#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours::object {
PagerProxy::PagerProxy(ustl::Arc<PortDispatcher> port, u64 const key, u64 const owner_id,
                       Overtime const overtime)
    : m_port(ustl::move(port)), m_key(key), m_owner_id(owner_id), m_overtime(overtime) {
    m_packet.origin = QueuedPacket::Origin::Kernel;
    m_packet.owner = this;
}

/// The cycle is made last, once both halves exist: the source holds the
/// proxy from its construction, and the proxy the source from here.
auto PagerProxy::create(ustl::Arc<PortDispatcher> port, u64 const key, u64 const owner_id,
                        Overtime const overtime, ustl::Arc<PagerProxy> *const proxy_out,
                        ustl::Arc<mm::PageSource> *const source_out) -> Status {
    auto proxy = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL)
                                            Self(ustl::move(port), key, owner_id, overtime));
    if (!proxy) {
        return Status::NoMemory;
    }
    ustl::Arc<mm::PageSource> source;
    if (auto const status = mm::PageSource::create(ustl::Arc<mm::PageProvider>(proxy), &source);
        Status::Ok != status) {
        return status;
    }
    {
        Guard guard{proxy->m_lock};
        proxy->m_source = source;
    }
    *proxy_out = ustl::move(proxy);
    *source_out = ustl::move(source);
    return Status::Ok;
}

/// By the time the last reference goes, the port has nothing of this
/// proxy's: the cycle is cut only once the packet is back for good.
PagerProxy::~PagerProxy() {
    DEBUG_ASSERT(!m_packet_out, "a pager proxy destroyed with its packet on the port");
    DEBUG_ASSERT(!m_complete_pending, "a pager proxy destroyed with its Complete pending");
    DEBUG_ASSERT(m_pending.empty(), "a pager proxy destroyed with requests to send");
}

auto PagerProxy::send_async_request(mm::PageRequest *const request) -> void {
    Guard guard{m_lock};
    DEBUG_ASSERT(!m_source_closed, "a closed source sends nothing");
    send_locked(request);
}

/// The request is the one the packet carries, or one waiting for it, or one
/// the packet carried into a port that had lost its handles and was never
/// read; only the first two are this proxy's to let go of.
auto PagerProxy::clear_async_request(mm::PageRequest *const request) -> void {
    Guard guard{m_lock};
    if (request == m_active) {
        m_active = nullptr;
        // Back from the port if it is still queued. If it is not, the port
        // has handed it out or dropped it and is about to call free(), which
        // finds no request on it.
        if (m_port->cancel_queued(&m_packet)) {
            m_packet_out = false;
            on_packet_back_locked();
        }
        return;
    }
    if (is_queued(*request)) {
        m_pending.erase(*request);
    }
}

/// Waits in rounds of warn_every, saying so in the log after each, and gives
/// up with TimeOut once give_up_after has passed: a pager that never answers
/// fails the faulting thread's access instead of holding it for ever. A
/// suspend does not end a wait that may not be interrupted by one; a kill
/// always does.
auto PagerProxy::wait_on_event(sync::Event *const event, bool const suspendable) -> Status {
    auto ignored = task::ThreadSignals::Suspend;
    if (suspendable) {
        ignored = task::ThreadSignals::None;
    }
    auto waited = Duration(0);
    for (;;) {
        auto const status = event->wait(
            time::Deadline::at(time::SteadyClock::now() + m_overtime.warn_every), ignored);
        if (Status::TimeOut != status) {
            return status;
        }
        waited += m_overtime.warn_every;
        auto const waited_ms = duration_cast<Milliseconds>(waited).count();
        if (waited >= m_overtime.give_up_after) {
            log::warn("pager: a page request on key {:#x} waited {} ms unanswered; it times out",
                      m_key, waited_ms);
            return Status::TimeOut;
        }
        log::warn("pager: a page request on key {:#x} has waited {} ms", m_key, waited_ms);
    }
}

/// The source sends nothing after this: its requests are all completed. The
/// Complete packet is the last one, sent after whatever is still out.
auto PagerProxy::on_detach() -> void {
    Guard guard{m_lock};
    DEBUG_ASSERT(!m_source_closed);
    m_complete_pending = true;
    send_locked(&m_complete);
}

/// The source is done with this proxy. If the port can still have the
/// packet -- the Complete is queued or waiting to be -- the cycle stays, and
/// free() of the Complete cuts it.
auto PagerProxy::on_close() -> void {
    // Dropped after the guard: declared before it.
    ustl::Arc<mm::PageSource> source;
    Guard guard{m_lock};
    m_source_closed = true;
    if (!m_complete_pending) {
        source = cut_cycle_locked();
    }
}

/// The port is done with the packet: a pager read it, or the port dropped it
/// as it lost its last handle. Either way it is this proxy's again.
auto PagerProxy::free(QueuedPacket *const packet) -> void {
    // Dropped after the guard: declared before it. It may be the last
    // reference to the source, whose destructor takes the source's lock and
    // drops the last reference to this proxy; nothing here touches the
    // proxy once the guard is gone.
    ustl::Arc<mm::PageSource> source;
    Guard guard{m_lock};
    DEBUG_ASSERT(packet == &m_packet && m_packet_out, "the port handed back a packet not out");
    m_packet_out = false;

    if (m_active == &m_complete) {
        m_active = nullptr;
        m_complete_pending = false;
        DEBUG_ASSERT(m_pending.empty(), "a request after the Complete packet");
    } else {
        m_active = nullptr;
        on_packet_back_locked();
    }

    if (m_source_closed && !m_complete_pending) {
        source = cut_cycle_locked();
    }
}

auto PagerProxy::send_locked(mm::PageRequest *const request) -> void {
    if (m_packet_out) {
        m_pending.push_back(*request);
        return;
    }

    auto &packet = m_packet.packet;
    packet = PortPacket{};
    packet.key = m_key;
    packet.type = PacketType::PageRequest;
    packet.status = Status::Ok;
    if (request == &m_complete) {
        packet.page_request.command = PageRequestCommand::Complete;
    } else {
        packet.page_request.command = PageRequestCommand::Read;
        packet.page_request.offset = offset(*request);
        packet.page_request.length = len(*request);
    }

    auto status = Status::BadHandle;
    if (!m_port_gone) {
        status = m_port->queue(&m_packet);
    }
    if (Status::Ok == status) {
        m_packet_out = true;
        m_active = request;
        return;
    }

    // The port has lost its last handle, and nothing sent to it is ever read
    // or handed back. The packet stays this proxy's. A Read is left to its
    // waiter, whose wait ends by overtime or by the detach that comes when
    // the VMO goes; a Complete is as good as delivered, or the cycle would
    // wait for a free() that never comes.
    DEBUG_ASSERT(Status::BadHandle == status);
    m_port_gone = true;
    if (request == &m_complete) {
        m_complete_pending = false;
    }
}

/// A loop, because a send into a port that has lost its handles leaves the
/// packet here: then the next request goes the same way, and the queue
/// drains.
auto PagerProxy::on_packet_back_locked() -> void {
    while (!m_packet_out) {
        auto front = m_pending.front();
        if (!front) {
            return;
        }
        auto &request = *front;
        m_pending.pop_front();
        send_locked(&request);
    }
}

auto PagerProxy::cut_cycle_locked() -> ustl::Arc<mm::PageSource> {
    DEBUG_ASSERT(m_source_closed && !m_complete_pending && !m_packet_out,
                 "the cycle is cut while the port may still have the packet");
    return ustl::move(m_source);
}
} // namespace ours::object
