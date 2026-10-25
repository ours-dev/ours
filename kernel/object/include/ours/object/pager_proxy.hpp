/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PAGER_PROXY_HPP
#define OURS_OBJECT_PAGER_PROXY_HPP 1

#include <ours/mm/page_request.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/clock.hpp>

#include <ustl/option.hpp>
#include <ustl/rc.hpp>

namespace ours::object {
/// The page provider of one pager VMO: it turns the VMO's page requests into
/// PageRequest packets on the pager's port, and a thread waiting on one into
/// a wait with an end.
///
/// The objects:
///   - m_packet, the one packet this proxy sends, embedded so that sending
///     allocates nothing: a request is sent under the VMO's and the
///     PageSource's locks. At most one request is out on the port at a time;
///     the ones that come while it is out wait on m_pending, and the next goes
///     out when the port hands the packet back (free()).
///   - m_active, the request the packet carries; m_complete, the request that
///     stands for the Complete packet, the last one a VMO sends.
///   - m_source, a reference to the PageSource over this proxy, which in turn
///     holds this proxy as its provider. The cycle is on purpose. The port
///     holds a plain pointer to m_packet while it is queued, and no
///     reference, so this proxy must not go while the packet is out; the
///     cycle keeps it, and is cut where the packet can no longer be out after
///     the source is closed: in on_close() when no Complete is pending, and
///     in free() of the Complete packet.
///
/// Locks: the PageSource calls every PageProvider method here with its own
/// lock held, so the order is PageSource, then this proxy's m_lock, then the
/// port's lock (queue(), cancel_queued()), the last a leaf. The port calls
/// free() with none of its locks held. m_source is dropped only after m_lock
/// is let go: ~PageSource takes the source's lock, which comes first.
class PagerProxy final: public mm::PageProvider, public PortPacketOwner {
    using Self = PagerProxy;

  public:
    /// How long a thread waits on a page request before the pager hears of it
    /// in the log, and before the wait gives up with TimeOut, so that a pager
    /// that never answers does not stop the faulting thread for ever.
    struct Overtime {
        Duration warn_every;
        Duration give_up_after;
    };
    static constexpr auto DEFAULT_OVERTIME =
        Overtime{.warn_every = Seconds(20), .give_up_after = Seconds(300)};

    /// Makes a proxy that sends its requests to |port| under |key|, and the
    /// PageSource over it, each holding the other. |owner_id| is what
    /// owner_id() answers: the pager's koid, so that a VMO made over the
    /// source can be told to be the pager's own.
    static auto create(ustl::Arc<PortDispatcher> port, u64 key, u64 owner_id, Overtime overtime,
                       ustl::Arc<PagerProxy> *proxy_out, ustl::Arc<mm::PageSource> *source_out)
        -> Status;

    ~PagerProxy() override;

    [[nodiscard]]
    auto owner_id() const -> ustl::Option<u64> override {
        return ustl::some(m_owner_id);
    }

  private:
    PagerProxy(ustl::Arc<PortDispatcher> port, u64 key, u64 owner_id, Overtime overtime);

    // mm::PageProvider, each called under the source's lock.
    auto send_async_request(mm::PageRequest *request) -> void override;
    auto clear_async_request(mm::PageRequest *request) -> void override;
    auto wait_on_event(sync::Event *event, bool suspendable) -> Status override;
    auto on_detach() -> void override;
    auto on_close() -> void override;

    // PortPacketOwner, called by the port with none of its locks held.
    auto free(QueuedPacket *packet) -> void override;

    /// Puts |request| in the packet and on the port, or on m_pending if the
    /// packet is out.
    TA_REQUIRES(m_lock) auto send_locked(mm::PageRequest *request) -> void;

    /// The packet is this proxy's again: the next request waiting goes out.
    TA_REQUIRES(m_lock) auto on_packet_back_locked() -> void;

    /// The source is closed and nothing of this proxy's is on the port: hands
    /// back the reference to the source, for the caller to drop once m_lock
    /// is let go.
    TA_REQUIRES(m_lock) auto cut_cycle_locked() -> ustl::Arc<mm::PageSource>;

    sync::Mutex m_lock;
    /// Set once, by the constructor. Not const: ustl::Arc passes its own
    /// constness through to the port, whose queue() changes it.
    ustl::Arc<PortDispatcher> m_port;
    u64 const m_key;
    u64 const m_owner_id;
    Overtime const m_overtime;

    QueuedPacket m_packet;
    /// Whether m_packet is on the port.
    bool m_packet_out TA_GUARDED(m_lock) = false;
    /// The request m_packet carries: null once the source has taken it back
    /// (clear_async_request()) while the packet was out.
    mm::PageRequest *m_active TA_GUARDED(m_lock) = nullptr;
    mm::PageRequest::ProviderList m_pending TA_GUARDED(m_lock);
    mm::PageRequest m_complete;
    /// The Complete packet is queued or waiting to be: the port may still
    /// have m_packet, so the cycle stays.
    bool m_complete_pending TA_GUARDED(m_lock) = false;
    bool m_source_closed TA_GUARDED(m_lock) = false;
    /// The port lost its last handle: nothing sent from now on is read.
    bool m_port_gone TA_GUARDED(m_lock) = false;
    ustl::Arc<mm::PageSource> m_source TA_GUARDED(m_lock);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PAGER_PROXY_HPP
