/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PAGE_REQUEST_HPP
#define OURS_MM_PAGE_REQUEST_HPP 1

#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/types.hpp>
#include <ours/types.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/pair.hpp>

namespace ours::mm {
class PageRequest;
class PageSource;

/// Whoever has the content of a sourced VMO's pages.
///
/// The kernel cannot make such a page on the spot, as it makes a zero page
/// for an anonymous VMO: the bytes are in a file, or in a user process that
/// serves them. So the PageSource hands each request it cannot merge into one
/// already outstanding to the provider, and the provider answers some time
/// later by supplying the pages to the VMO (VmObject::supply_pages()), or by
/// failing them (PageSource::on_pages_failed()).
///
/// The source calls every method here with its own lock held, so none of
/// them may call back into the source or the VMO. A provider that has a
/// thread of its own copies what it needs out of a request under its own lock
/// (offset() and len() below) and does not touch the request after letting go
/// of it: from then on the faulting thread can see it completed or cancel it,
/// and return, and the request lives on that thread's stack.
class PageProvider: public ustl::RefCounted<PageProvider, ustl::sync::ThreadSafePolicy> {
    using Self = PageProvider;

  public:
    ~PageProvider() override = default;

    /// An id chosen by whoever made the provider, so that it can tell later
    /// that a VMO's pages come from one of its own: a pager, the koid of its
    /// object. Opaque to mm, never changes, and none for a provider nobody
    /// asks about.
    [[nodiscard]]
    virtual auto owner_id() const -> ustl::Option<u64> {
        return ustl::NONE;
    }

  protected:
    /// What a request handed to the provider asks for, in bytes of the VMO.
    /// Only valid between send_async_request() and clear_async_request().
    static auto offset(PageRequest const &request) -> u64;
    static auto len(PageRequest const &request) -> u64;

    /// Whether |request| is on a PageRequest::ProviderList: a provider that
    /// takes requests off its queue before serving them asks this in
    /// clear_async_request(), where the request may be queued or not.
    static auto is_queued(PageRequest const &request) -> bool;

  private:
    friend class PageSource;

    /// The source has a request it cannot merge into an outstanding one. The
    /// provider queues it and answers it later; it must not block here.
    virtual auto send_async_request(PageRequest *request) -> void = 0;

    /// The request is complete or withdrawn. The provider forgets it if it
    /// still has it queued. Called exactly once for every request the
    /// provider holds: one sent, or one handed to it by swap_async_request(),
    /// and never for the request that call took away.
    virtual auto clear_async_request(PageRequest *request) -> void = 0;

    /// |old|, a request the provider holds, is withdrawn, and |heir|, which
    /// was waiting on it, asks for the same range in its stead. Whatever the
    /// provider has of |old| is now |heir|'s: its place in the queue, or the
    /// work of answering it. A clear of |old| followed by a send of |heir|
    /// would lose that place, or ask for a range again that is already being
    /// answered.
    virtual auto swap_async_request(PageRequest *old, PageRequest *heir) -> void = 0;

    /// How a faulting thread sleeps until its request is complete. The
    /// provider decides, because only it knows whether the wait may be
    /// interrupted, and a user pager may want to report a waiter that never
    /// wakes.
    virtual auto wait_on_event(sync::Event *event, bool suspendable) -> Status = 0;

    /// The VMO no longer takes pages: every outstanding request has been
    /// completed, and no new one will be sent.
    virtual auto on_detach() -> void = 0;

    /// The VMO is gone. Always after on_detach().
    virtual auto on_close() -> void = 0;
};

/// One request for pages that are not there yet, raised by a page fault (or
/// anything else that needs the content) and waited on by the thread that
/// raised it.
///
/// It lives on the stack of that thread, because the fault path must not
/// allocate: an allocation can recurse into reclaim and fault again. So every
/// container it goes into is intrusive, and the request has to be taken out
/// of all of them before its thread returns -- which is what the destructor
/// does if nobody completed it.
///
/// A raised request is one of two kinds. A primary is in its source's set of
/// outstanding requests and has been sent to the provider. An overlap is
/// waiting on a primary that already asks for its first page: it is on that
/// primary's list instead, and the provider never sees it. Completing a
/// primary completes its overlaps.
///
/// Every field is guarded by the source's lock while the request is raised,
/// and belongs to the owning thread otherwise.
class PageRequest {
    using Self = PageRequest;

  public:
    PageRequest() = default;

    /// Withdraws the request if it is still raised.
    ~PageRequest();

    PageRequest(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Blocks until the pages this request asked for have been supplied or
    /// failed, and says which. Called with no lock held, after the source
    /// answered ShouldWait. A wait that ends any other way (the thread was
    /// interrupted) withdraws the request.
    auto wait(bool suspendable) -> Status;

    /// Withdraws the request; nothing if it is not raised.
    auto cancel() -> void;

  private:
    friend class PageSource;
    friend class PageProvider;

    /// m_offset of a request that is not raised: never raised, or completed,
    /// or withdrawn.
    static constexpr u64 NOT_RAISED = ~u64{0};

    [[nodiscard]]
    auto is_raised() const -> bool {
        return m_offset != NOT_RAISED;
    }

    [[nodiscard]]
    auto end() const -> u64 {
        return m_offset + m_len;
    }

    /// Whether [start, end) shares a byte with this request.
    [[nodiscard]]
    auto overlaps(u64 const start, u64 const end) const -> bool {
        return end > m_offset && start < this->end();
    }

    /// [start, end) cut down to this request and moved into its own
    /// coordinates, where 0 is m_offset. The range must overlap it.
    [[nodiscard]]
    auto trim_to_request(u64 start, u64 end) const -> ustl::Pair<u64, u64>;

    /// Where the request starts, in bytes of the VMO; NOT_RAISED when it is not
    /// raised.
    u64 m_offset = NOT_RAISED;
    u64 m_len = 0;

    /// How much of a primary is still to be supplied. A supply may cover only
    /// part of it, and the request completes when this reaches zero.
    u64 m_pending = 0;

    /// What wait() reports: Ok, or why the pages failed.
    Status m_status = Status::Ok;

    /// Between send_async_request() and clear_async_request(), when the
    /// provider may read the request and nothing may change its range.
    bool m_provider_owned = false;

    /// Who the request was raised against. A reference, so that a source
    /// whose VMO went away while this thread slept is still there to be
    /// waited on and withdrawn from.
    ustl::Arc<PageSource> m_source;

    /// Signalled once, when the request completes; AutoUnsignal, so that the
    /// next request raised in this object waits again.
    sync::Event m_event{sync::Event::Options::AutoUnsignal};

    /// A primary is in its source's set by this hook; an overlap is on its
    /// primary's list by the next one. Never both.
    ustl::collections::intrusive::SetMemberHook<> m_source_hook;
    ustl::collections::intrusive::ListMemberHook<> m_overlap_hook;

    USTL_DECLARE_HOOK_OPTION(Self, m_overlap_hook, OverlapOption);
    USTL_DECLARE_LIST(Self, OverlapList, OverlapOption);

    /// The overlaps waiting on this request, when it is a primary.
    OverlapList m_overlap;

    /// The provider's, for the queue it keeps of requests sent to it.
    ustl::collections::intrusive::ListMemberHook<> m_provider_hook;

    /// The order of the source's set: by end. Two primaries never overlap, so
    /// the end orders them as well as the start would, and the first primary
    /// that can contain an offset is the first whose end is past it.
    struct ByEnd {
        auto operator()(Self const &left, Self const &right) const noexcept -> bool {
            return left.end() < right.end();
        }
    };
    USTL_DECLARE_HOOK_OPTION(Self, m_source_hook, SourceOption);
    using Outstanding =
        ustl::collections::intrusive::Set<Self, SourceOption,
                                          ustl::collections::intrusive::Compare<ByEnd>>;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_provider_hook, ProviderOption);

    /// A provider's queue of requests sent to it.
    USTL_DECLARE_LIST(Self, ProviderList, ProviderOption);
};

inline auto PageProvider::offset(PageRequest const &request) -> u64 {
    DEBUG_ASSERT(request.m_provider_owned);
    return request.m_offset;
}

inline auto PageProvider::len(PageRequest const &request) -> u64 {
    DEBUG_ASSERT(request.m_provider_owned);
    return request.m_len;
}

inline auto PageProvider::is_queued(PageRequest const &request) -> bool {
    return request.m_provider_hook.is_linked();
}

/// A wait for a blank frame, raised when the PMM would not hand one to a VMO
/// while free memory is short (alloc_frames() answered ShouldWait for an
/// allocation with Gaf::CanWait).
///
/// Unlike a PageRequest it asks no one for content: any frame will do, so
/// it has no offset, no source and no provider, and two of them never merge.
/// The wait gets the frame itself and keeps it, and the call that raised the
/// request, retried, takes that frame instead of asking the PMM again, which
/// could answer ShouldWait once more and starve the waiter.
///
/// It lives on the stack of the thread that waits, beside its PageRequest in
/// a MultiPageRequest, and is used by that thread alone.
class AnonymousPageRequest {
    using Self = AnonymousPageRequest;

  public:
    AnonymousPageRequest() = default;

    /// Gives back a frame the wait got that no retry took: the retry found
    /// the page there already, or never came.
    ~AnonymousPageRequest();

    AnonymousPageRequest(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Raises the request: the next wait() waits for a frame allocated with
    /// |alloc_flags|, the flags of the allocation that was turned away.
    auto make_active(Gaf alloc_flags) -> void;

    [[nodiscard]]
    auto is_active() const -> bool {
        return m_active;
    }

    /// Sleeps, with no lock held, until the PMM hands out a frame, and keeps
    /// it for the retried call. Ok with a frame kept, or why there is none:
    /// OutOfMem when delayed allocations are refused, or the status of an
    /// interrupted wait. Each lost race (ShouldWait) waits again, and every
    /// 5 seconds of waiting is reported. The request is not raised after it.
    auto allocate(task::Interruptible interruptible) -> Status;

    [[nodiscard]]
    auto has_frame() const -> bool {
        return m_frame != nullptr;
    }

    /// The frame the wait got, for the retried call to use, and the request
    /// no longer holds it.
    [[nodiscard]]
    auto take_frame() -> PmFrame * {
        auto *const frame = m_frame;
        m_frame = nullptr;
        return frame;
    }

  private:
    bool m_active = false;
    Gaf m_alloc_flags = Gaf{};
    PmFrame *m_frame = nullptr;
};

/// A bundle of the typed sub-requests one page fault may need to raise: the
/// caller hands the whole bundle down and each layer picks out the slot it
/// knows how to fill.
class MultiPageRequest {
    using Self = MultiPageRequest;

  public:
    auto wait(bool suspendable) -> Status;

    auto get_anonymous() -> AnonymousPageRequest * {
        return &m_anonymous;
    }

    auto get_read() -> PageRequest * {
        return &m_read;
    }

  private:
    AnonymousPageRequest m_anonymous;
    PageRequest m_read;
};

/// The kernel's side of a VMO whose pages come from a provider: it records
/// which ranges have been asked for, so that a second fault on a page already
/// asked for waits on the first request instead of asking again, and it wakes
/// the waiters when the pages arrive.
///
/// One per sourced VMO, held by the root of its page tree and by every
/// request raised against it.
///
/// Lock order: the VMO's lock, then this source's, then the provider's.
/// request_pages() is called with the VMO's lock held, and so is
/// on_pages_supplied(), from supply_pages().
class PageSource final: public ustl::RefCounted<PageSource, ustl::sync::ThreadSafePolicy> {
    using Self = PageSource;

  public:
    static auto create(ustl::Arc<PageProvider> provider, ustl::Arc<PageSource> *out) -> Status;

    /// Raises |request| for [offset, offset + len), page aligned and not empty,
    /// or makes it wait on an outstanding request that already asks for
    /// |offset|. Answers ShouldWait, after which the caller drops its locks
    /// and calls request->wait(); or BadState once the source is detached.
    ///
    /// The request may be cut short, to end where an outstanding one ends or
    /// where the next one starts: it never asks for a page twice. The caller
    /// retries once it wakes and raises another for what is still missing.
    auto request_pages(u64 offset, u64 len, PageRequest *request) -> Status;

    /// [offset, offset + len) now holds pages it did not hold before. Every
    /// request this completes is woken.
    auto on_pages_supplied(u64 offset, u64 len) -> void;

    /// The provider cannot supply [offset, offset + len). Every request this
    /// touches is completed with |status|, which is what the faulting thread
    /// sees.
    auto on_pages_failed(u64 offset, u64 len, Status status) -> void;

    /// The VMO stops taking pages: every outstanding request is completed --
    /// its waiter retries and finds the source detached -- and no new one is
    /// raised.
    auto detach() -> void;

    /// The VMO is gone. Detaches first.
    auto close() -> void;

    [[nodiscard]]
    auto is_detached() const -> bool;

    /// The provider's owner_id(). The provider is set once, by the
    /// constructor, so this takes no lock.
    [[nodiscard]]
    auto owner_id() const -> ustl::Option<u64>;

    /// How many requests are raised, primaries and overlaps together. For
    /// tests, which need to know that every thread they started is waiting.
    [[nodiscard]]
    auto nr_raised() const -> usize;

  private:
    friend class PageRequest;
    friend class ustl::RefCounted<PageSource, ustl::sync::ThreadSafePolicy>;

    explicit PageSource(ustl::Arc<PageProvider> provider);
    ~PageSource() override;

    auto wait_on_request(PageRequest *request, bool suspendable) -> Status;
    auto cancel_request(PageRequest *request) -> void;

    TA_REQUIRES(m_lock)
    auto populate_request_locked(u64 offset, u64 len, PageRequest *request) -> void;
    TA_REQUIRES(m_lock) auto send_request_locked(PageRequest *request) -> void;
    TA_REQUIRES(m_lock) auto complete_request_locked(PageRequest *request) -> void;
    TA_REQUIRES(m_lock) auto resolve_requests_locked(u64 offset, u64 len, Status status) -> void;
    TA_REQUIRES(m_lock) auto cancel_request_locked(PageRequest *request) -> void;

    /// The outstanding primary that contains |offset|, or the set's end.
    TA_REQUIRES(m_lock)
    auto primary_containing_locked(u64 offset) -> PageRequest::Outstanding::IterMut;

    /// The lifecycle bits are mutually exclusive. None accepts requests.
    /// Under m_lock, detach() sets Detached and drains requests. close()
    /// replaces Detached with Closed after the provider's detach callback.
    /// Both bits stop new requests. Only these transitions write the flags.
    enum class Flags : u8 {
        None = 0,
        Detached = 0_bit,
        Closed = 1_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    TA_REQUIRES(m_lock) auto is_detached_locked() const -> bool {
        return !!(m_flags & (Flags::Detached | Flags::Closed));
    }

    mutable sync::Mutex m_lock;
    Flags m_flags TA_GUARDED(m_lock) = Flags::None;
    PageRequest::Outstanding m_outstanding TA_GUARDED(m_lock);
    /// Set once, by the constructor. Not const: ustl::Arc passes its own
    /// constness through to the provider, whose methods change it.
    ustl::Arc<PageProvider> m_provider;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PAGE_REQUEST_HPP
