/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/page_request.hpp>

#include <algorithm>

#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
namespace {
    /// The key comparison for PageRequest::Outstanding: whether |offset| comes
    /// before the end of |request|. upper_bound() with it finds the first
    /// primary whose end is past |offset|, the only one that can contain it.
    struct EndIsPast {
        auto operator()(u64 const offset, u64 const end) const noexcept -> bool {
            return offset < end;
        }
    };
} // namespace

auto PageRequest::trim_to_request(u64 const start, u64 const end) const -> ustl::Pair<u64, u64> {
    DEBUG_ASSERT(overlaps(start, end));
    auto const first = start > m_offset ? start - m_offset : 0;
    auto const last = end < this->end() ? end - m_offset : m_len;
    return {first, last};
}

PageRequest::~PageRequest() {
    cancel();
}

/// m_source is read without the source's lock: only the owning thread sets
/// it, through get_pages(), and only the owning thread calls this.
auto PageRequest::cancel() -> void {
    if (!m_source) {
        return;
    }
    m_source->cancel_request(this);
}

auto PageRequest::wait(bool const suspendable) -> Status {
    DEBUG_ASSERT(m_source, "waited on a request that was never raised");

    auto const status = m_source->wait_on_request(this, suspendable);
    // A request completed with a failure is no longer raised, and withdrawing
    // it does nothing. One whose wait was cut short -- the thread was
    // interrupted -- is still raised, and nobody will wait on it now.
    if (Status::Ok != status) {
        m_source->cancel_request(this);
    }
    return status;
}

auto MultiPageRequest::wait(bool const suspendable) -> Status {
    return m_read.wait(suspendable);
}

auto PageSource::create(ustl::Arc<PageProvider> provider, ustl::Arc<PageSource> *const out)
    -> Status {
    DEBUG_ASSERT(provider);
    auto *const source = new (GAF_KERNEL) PageSource(ustl::move(provider));
    if (!source) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(source);
    return Status::Ok;
}

PageSource::PageSource(ustl::Arc<PageProvider> provider) : m_provider(ustl::move(provider)) {}

/// A source outlives its VMO only as long as some request raised against it
/// does, and the VMO closes it when it goes, so by now nothing is outstanding.
/// One that was never closed is a VMO that forgot to.
PageSource::~PageSource() {
    Guard guard{m_lock};
    DEBUG_ASSERT(m_detached && m_closed, "a page source destroyed without being closed");
    DEBUG_ASSERT(m_outstanding.empty());
}

auto PageSource::get_pages(u64 const offset, u64 const len, PageRequest *const request) -> Status {
    DEBUG_ASSERT(request);
    DEBUG_ASSERT(len > 0);
    DEBUG_ASSERT(ustl::mm::is_aligned(offset, PAGE_SIZE) && ustl::mm::is_aligned(len, PAGE_SIZE));
    DEBUG_ASSERT(offset + len > offset, "the range wraps");

    Guard guard{m_lock};
    if (m_detached) {
        return Status::BadState;
    }

    // A request answered ShouldWait is waited on before it is raised again:
    // wait() leaves it completed or withdrawn.
    DEBUG_ASSERT(!request->is_raised(), "a request raised again before it was waited on");
    populate_request_locked(offset, len, request);
    return Status::ShouldWait;
}

auto PageSource::primary_containing_locked(u64 const offset) -> PageRequest::Outstanding::IterMut {
    auto const end_is_past = [](u64 const key, PageRequest const &request) noexcept {
        return EndIsPast{}(key, request.end());
    };
    auto it = m_outstanding.upper_bound(offset, end_is_past);
    if (it != m_outstanding.end() && it->m_offset <= offset) {
        return it;
    }
    return m_outstanding.end();
}

/// The request is cut so that it never asks for a page another primary asks
/// for. Two cases, by where it starts:
///
///   - inside an outstanding primary: it waits on that one, and asks for no
///     more than the primary does, so its end is clipped to the primary's;
///   - before one, and running into it: it stops where that one starts.
///
/// Either way the waiter wakes with its first page asked for, which is the
/// page a fault needs; what it lost off the end it asks for again on the
/// retry if it still needs it.
auto PageSource::populate_request_locked(u64 const offset, u64 const len,
                                         PageRequest *const request) -> void {
    request->m_source = ustl::Arc<Self>::adopt(this);
    request->m_offset = offset;
    request->m_len = len;
    request->m_status = Status::Ok;
    // A completion this request was signalled with and never waited on --
    // it was withdrawn instead -- must not satisfy the wait for this one.
    request->m_event.unsignal();

    auto const end = offset + len;
    auto const end_is_past = [](u64 const key, PageRequest const &other) noexcept {
        return EndIsPast{}(key, other.end());
    };
    auto const next = m_outstanding.upper_bound(offset, end_is_past);
    if (next != m_outstanding.end()) {
        if (offset >= next->m_offset) {
            request->m_len = std::min(end, next->end()) - offset;
        } else if (end > next->m_offset) {
            request->m_len = next->m_offset - offset;
        }
    }

    send_request_locked(request);
}

auto PageSource::send_request_locked(PageRequest *const request) -> void {
    auto const primary = primary_containing_locked(request->m_offset);
    if (primary != m_outstanding.end()) {
        // Contained, by populate_request_locked(): what it asks for is already
        // asked for.
        DEBUG_ASSERT(request->end() <= primary->end());
        primary->m_overlap.push_back(*request);
        return;
    }

    request->m_pending = request->m_len;
    [[maybe_unused]] auto const [_, inserted] = m_outstanding.insert(*request);
    DEBUG_ASSERT(inserted, "two primaries end at the same offset");
    request->m_provider_owned = true;
    m_provider->send_async_request(request);
}

/// Taken back from the provider before anyone is woken: once a waiter runs it
/// may raise the request again, and the provider must not be holding it then.
///
/// Every request here may be freed by its thread as soon as it is signalled,
/// but not before this returns: the thread's next step is ~PageRequest(),
/// which takes this source's lock to withdraw it, and the lock is held here.
auto PageSource::complete_request_locked(PageRequest *const request) -> void {
    DEBUG_ASSERT(request->m_provider_owned);
    DEBUG_ASSERT(!request->m_source_hook.is_linked());

    m_provider->clear_async_request(request);
    request->m_provider_owned = false;

    while (auto waiter = request->m_overlap.front()) {
        request->m_overlap.pop_front();
        waiter->m_offset = PageRequest::NOT_RAISED;
        waiter->m_event.signal(waiter->m_status);
    }
    request->m_offset = PageRequest::NOT_RAISED;
    request->m_event.signal(request->m_status);
}

/// Every primary [offset, offset + len) touches has that much less to wait
/// for; the ones with nothing left are completed.
///
/// A failure is recorded only on the requests whose first page it covers.
/// A waiter needs its first page and nothing past it: a failure further on is
/// not its failure, and the retry asks for those pages again if they are
/// still needed.
auto PageSource::resolve_requests_locked(u64 const offset, u64 const len, Status const status)
    -> void {
    if (m_detached) {
        return;
    }

    auto const end = offset + len;
    auto const end_is_past = [](u64 const key, PageRequest const &request) noexcept {
        return EndIsPast{}(key, request.end());
    };
    auto it = m_outstanding.upper_bound(offset, end_is_past);
    while (it != m_outstanding.end() && it->m_offset < end) {
        auto &request = *it;
        ++it;

        auto const [first, last] = request.trim_to_request(offset, end);
        if (Status::Ok != status) {
            if (0 == first) {
                request.m_status = status;
            }
            for (auto &waiter: request.m_overlap) {
                if (waiter.overlaps(offset, end) &&
                    0 == waiter.trim_to_request(offset, end).first) {
                    waiter.m_status = status;
                }
            }
        }

        // More than is pending is a range supplied twice, once after part of
        // it was taken away again. Not an error; the request is done.
        auto const resolved = last - first;
        if (resolved < request.m_pending) {
            request.m_pending -= resolved;
            continue;
        }

        m_outstanding.erase(request);
        complete_request_locked(&request);
    }
}

auto PageSource::on_pages_supplied(u64 const offset, u64 const len) -> void {
    Guard guard{m_lock};
    resolve_requests_locked(offset, len, Status::Ok);
}

auto PageSource::on_pages_failed(u64 const offset, u64 const len, Status const status) -> void {
    DEBUG_ASSERT(Status::Ok != status && Status::ShouldWait != status,
                 "a failure that is not one: {}", to_string(status));

    Guard guard{m_lock};
    resolve_requests_locked(offset, len, status);
}

/// Completed with Ok, not with a failure: the waiters retry, and the retry is
/// what finds the source detached and fails, from get_pages().
auto PageSource::detach() -> void {
    Guard guard{m_lock};
    if (m_detached) {
        return;
    }
    m_detached = true;

    while (!m_outstanding.empty()) {
        auto &request = *m_outstanding.begin();
        m_outstanding.erase(request);
        complete_request_locked(&request);
    }

    m_provider->on_detach();
}

auto PageSource::close() -> void {
    detach();

    Guard guard{m_lock};
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_provider->on_close();
}

auto PageSource::is_detached() const -> bool {
    Guard guard{m_lock};
    return m_detached;
}

auto PageSource::nr_raised() const -> usize {
    Guard guard{m_lock};
    usize count = 0;
    for (auto const &primary: m_outstanding) {
        count += 1 + primary.m_overlap.size();
    }
    return count;
}

auto PageSource::wait_on_request(PageRequest *const request, bool const suspendable) -> Status {
    return m_provider->wait_on_event(&request->m_event, suspendable);
}

auto PageSource::cancel_request(PageRequest *const request) -> void {
    Guard guard{m_lock};
    cancel_request_locked(request);
}

/// Three cases, by what the request is:
///
///   - an overlap: it leaves its primary's list, and nothing else changes;
///   - a primary with overlaps: the first overlap takes its place, range and
///     all, so the pages are still asked for on behalf of those who wait. The
///     provider is told by taking the old request back and sending the new
///     one. If it had already started on the old one, it supplies the pages
///     twice, and the second supply finds them there and gives them back;
///   - a primary alone: it leaves the set and the provider.
auto PageSource::cancel_request_locked(PageRequest *const request) -> void {
    if (!request->is_raised()) {
        return;
    }

    if (request->m_overlap_hook.is_linked()) {
        auto const primary = primary_containing_locked(request->m_offset);
        ASSERT(primary != m_outstanding.end(), "an overlap with no primary to wait on");
        primary->m_overlap.erase(*request);
    } else if (auto first = request->m_overlap.front()) {
        auto &heir = *first;
        request->m_overlap.pop_front();
        heir.m_overlap.swap(request->m_overlap);
        heir.m_offset = request->m_offset;
        heir.m_len = request->m_len;
        heir.m_pending = request->m_pending;

        m_outstanding.erase(*request);
        m_provider->clear_async_request(request);
        request->m_provider_owned = false;

        [[maybe_unused]] auto const [_, inserted] = m_outstanding.insert(heir);
        DEBUG_ASSERT(inserted, "two primaries end at the same offset");
        heir.m_provider_owned = true;
        m_provider->send_async_request(&heir);
    } else {
        m_outstanding.erase(*request);
        m_provider->clear_async_request(request);
        request->m_provider_owned = false;
    }

    request->m_offset = PageRequest::NOT_RAISED;
}

} // namespace ours::mm
