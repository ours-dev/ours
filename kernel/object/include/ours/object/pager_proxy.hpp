/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PAGER_PROXY_HPP
#define OURS_OBJECT_PAGER_PROXY_HPP 1

#include <ours/mm/page_request.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/clock.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>

namespace ours::object {
class PagerDispatcher;

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
///     cycle keeps it, and is cut (cut_if_done_locked()) once the source is
///     closed and the packet can no longer be out: in on_close(), in free(),
///     and when the pager closes (on_pager_close()).
///   - m_pager, the pager that made this proxy, and m_pager_ref, the pager's
///     reference to it while it is on the pager's list. The pager lets go of
///     it when the cycle is cut, or when it closes.
///
/// Locks: the PageSource calls every PageProvider method here with its own
/// lock held, so the order is PageSource, then this proxy's m_lock, then the
/// pager's list lock or the port's lock, both leaves. The port calls free()
/// with none of its locks held. m_source and m_pager_ref are dropped only
/// after m_lock is let go: ~PageSource takes the source's lock, which comes
/// first, and the last reference to this proxy may be among them.
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
    /// source can be told to be the pager's own. |pager|, null for a proxy no
    /// pager keeps, is the pager it goes on the list of.
    ///
    /// Takes no lock of the proxy's: the pager calls this under its list
    /// lock, which comes after the proxy's, and nothing else can reach the
    /// proxy until the source is handed out.
    static auto create(ustl::Arc<PortDispatcher> port, u64 key, u64 owner_id, Overtime overtime,
                       PagerDispatcher *pager, ustl::Arc<PagerProxy> *proxy_out,
                       ustl::Arc<mm::PageSource> *source_out) -> Status;

    ~PagerProxy() override;

    [[nodiscard]]
    auto owner_id() const -> ustl::Option<u64> override {
        return ustl::some(m_owner_id);
    }

    /// How many proxies exist, for the tests that check the cycle is cut.
    [[nodiscard]]
    static auto nr_alive() -> usize;

  private:
    friend class PagerDispatcher;

    PagerProxy(ustl::Arc<PortDispatcher> port, u64 key, u64 owner_id, Overtime overtime,
               PagerDispatcher *pager);

    // mm::PageProvider, each called under the source's lock.
    auto send_async_request(mm::PageRequest *request) -> void override;
    auto clear_async_request(mm::PageRequest *request) -> void override;
    auto swap_async_request(mm::PageRequest *old, mm::PageRequest *heir) -> void override;
    auto wait_on_event(sync::Event *event, bool suspendable) -> Status override;
    auto on_detach() -> void override;
    auto on_close() -> void override;

    // PortPacketOwner, called by the port with none of its locks held.
    auto free(QueuedPacket *packet) -> void override;

    /// The pager is gone: closes the source if it is not closed yet, so no
    /// request is sent that nobody will answer, and takes back whatever is
    /// still on the port, the Complete included. Called by the pager with no
    /// lock held, once it has taken this proxy off its list.
    auto on_pager_close() -> void;

    /// Puts |request| in the packet and on the port, or on m_pending if the
    /// packet is out.
    TA_REQUIRES(m_lock) auto send_locked(mm::PageRequest *request) -> void;

    /// The packet is this proxy's again: the next request waiting goes out.
    TA_REQUIRES(m_lock) auto on_packet_back_locked() -> void;

    /// What cutting the cycle hands back, to be dropped once m_lock is let go.
    struct Cut {
        ustl::Arc<mm::PageSource> source;
        ustl::Arc<PagerProxy> pager_ref;
    };

    /// Cuts the cycle if the source is closed and the port can no longer have
    /// the packet: nothing out, no Complete still to go. Empty otherwise, or
    /// when it is cut already.
    TA_REQUIRES(m_lock) auto cut_if_done_locked() -> Cut;

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
    /// (clear_async_request()) while the packet was out, and the request
    /// that took its place if the source handed it on
    /// (swap_async_request()).
    mm::PageRequest *m_active TA_GUARDED(m_lock) = nullptr;
    mm::PageRequest::ProviderList m_pending TA_GUARDED(m_lock);
    mm::PageRequest m_complete;
    /// The Complete packet is queued or waiting to be: the port may still
    /// have m_packet, so the cycle stays.
    bool m_complete_pending TA_GUARDED(m_lock) = false;
    bool m_source_closed TA_GUARDED(m_lock) = false;
    /// The port lost its last handle: nothing sent from now on is read.
    bool m_port_gone TA_GUARDED(m_lock) = false;
    /// The pager closed: nobody will read this proxy's packets, so a Complete
    /// not yet out is dropped rather than sent.
    bool m_pager_closed TA_GUARDED(m_lock) = false;
    ustl::Arc<mm::PageSource> m_source TA_GUARDED(m_lock);

    /// The pager that keeps this proxy, null once it has closed or let go of
    /// it. Read under m_lock; the pager outlives every call made through it,
    /// since it clears this, under this lock, before its own end.
    PagerDispatcher *m_pager TA_GUARDED(m_lock);

    /// The pager's reference to this proxy while it is on the pager's list:
    /// kept here because an intrusive list holds no references. Guarded by
    /// the pager's list lock, and moved out with the hook.
    ustl::Arc<PagerProxy> m_pager_ref;
    ustl::collections::intrusive::ListMemberHook<> m_pager_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_pager_hook, PagerOption);

  public:
    USTL_DECLARE_LIST(Self, PagerList, PagerOption);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PAGER_PROXY_HPP
