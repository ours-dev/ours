/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/object/pager_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/vmo.hpp>

#include <utility>

#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

namespace ours::object {
auto PagerDispatcher::create(u32 const options, KernelHandle<Self> &handle_out, Rights &rights_out)
    -> Status {
    if (options != 0) {
        return Status::InvArgs;
    }

    // ALLOC_USER, as with every other dispatcher: the lifetime of this object
    // is driven by how many handles userspace holds.
    auto self = ustl::Arc<Self>::adopt(new (mm::ALLOC_USER) Self());
    if (!self) {
        return Status::OutOfMem;
    }
    rights_out = default_right();
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

/// Every proxy has left the list by now: on_zero_handles() took the ones
/// still on it, and it ran before the last reference could go.
PagerDispatcher::~PagerDispatcher() {
    DEBUG_ASSERT(m_proxies.empty(), "a pager destroyed with proxies on its list");
}

/// The proxy and its source are made, and the proxy put on the list, in one
/// hold of the list lock after the zero-handles check: a close that comes
/// after finds the proxy on the list and closes it, and one that came before
/// is seen here. The VMO is made over the source after the lock is let go.
auto PagerDispatcher::create_vmo(VmoOptions const options, ustl::Arc<PortDispatcher> port,
                                 u64 const key, u64 const size,
                                 KernelHandle<VmoDispatcher> &handle_out, Rights &rights_out)
    -> Status {
    m_canary.verify<Self>();

    // Options first, as VmoDispatcher::create_user() takes them: an unknown
    // bit is the caller's mistake, and Resizable a known one this kernel
    // defers.
    constexpr auto KNOWN = std::to_underlying(VmoOptions::Resizable);
    auto const bits = std::to_underlying(options);
    if ((bits & ~KNOWN) != 0) {
        return Status::InvArgs;
    }
    if (bits != 0) {
        return Status::NotSupported;
    }

    // Checked before rounding: MAX_SIZE leaves a page of headroom below the
    // top of the range, so anything that passes rounds up without wrapping.
    if (size > mm::VmObjectPaged::MAX_SIZE) {
        return Status::OutOfRange;
    }
    auto const rounded = ustl::round_up(size, PAGE_SIZE);

    ustl::Arc<mm::PageSource> source;
    {
        Guard guard{m_list_lock};
        if (m_zero_handles) {
            return Status::BadState;
        }
        ustl::Arc<PagerProxy> proxy;
        if (auto const status = PagerProxy::create(
                ustl::move(port), key, koid(), PagerProxy::DEFAULT_OVERTIME, this, &proxy, &source);
            Status::Ok != status) {
            return status;
        }
        m_proxies.push_back(*proxy);
        proxy->m_pager_ref = ustl::move(proxy);
    }

    // create_external() closes the source if the VMO cannot be made, which sends
    // Complete for a VMO nobody was handed (the user pager design's open
    // question); the proxy leaves the list once the pager reads it.
    ustl::Arc<mm::VmObjectPaged> vmo;
    if (auto const status = mm::VmObjectPaged::create_external(source, mm::ALLOC_USER_VMO, rounded,
                                                               mm::VmoFlags::Lazy, &vmo);
        Status::Ok != status) {
        return status;
    }
    return VmoDispatcher::create(ustl::move(vmo), handle_out, rights_out);
}

auto PagerDispatcher::owns(VmoDispatcher &vmo) const -> bool {
    return vmo.pager_koid() == koid();
}

auto PagerDispatcher::supply_pages(VmoDispatcher &vmo, u64 const offset, u64 const length,
                                   VmoDispatcher &aux, u64 const aux_offset) -> Status {
    m_canary.verify<Self>();

    if (!owns(vmo)) {
        return Status::InvArgs;
    }
    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(length, PAGE_SIZE) ||
        !ustl::is_aligned(aux_offset, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    if (length == 0) {
        return Status::Ok;
    }

    // One tree and then the other: the pages are out of |aux| before |vmo|'s
    // lock is taken. A failure of the supply frees what the list still has.
    // Copies of the references: ustl::Arc passes const on, and vmo() hands
    // out a const one.
    auto source = aux.vmo();
    auto target = vmo.vmo();
    mm::VmPageSpliceList pages(length / PAGE_SIZE);
    if (auto const status = source->take_pages(aux_offset, length, &pages); Status::Ok != status) {
        return status;
    }
    return target->supply_pages(offset, length, &pages);
}

auto PagerDispatcher::detach_vmo(VmoDispatcher &vmo) -> Status {
    m_canary.verify<Self>();

    if (!owns(vmo)) {
        return Status::InvArgs;
    }
    auto target = vmo.vmo();
    target->detach_source();
    return Status::Ok;
}

/// One proxy at a time: taken off the list under the lock, with the list's
/// reference, and closed with the lock let go, since a proxy's lock comes
/// before it. A proxy that cuts its cycle meanwhile finds itself off the
/// list (release_proxy()), and the reference taken here keeps it alive until
/// its close is done.
auto PagerDispatcher::on_zero_handles() -> void {
    {
        Guard guard{m_list_lock};
        m_zero_handles = true;
    }
    for (;;) {
        ustl::Arc<PagerProxy> proxy;
        {
            Guard guard{m_list_lock};
            auto front = m_proxies.front();
            if (!front) {
                break;
            }
            m_proxies.erase(*front);
            proxy = ustl::move(front->m_pager_ref);
        }
        proxy->on_pager_close();
    }
}

auto PagerDispatcher::release_proxy(PagerProxy *const proxy) -> ustl::Arc<PagerProxy> {
    Guard guard{m_list_lock};
    if (!proxy->m_pager_hook.is_linked()) {
        return {};
    }
    m_proxies.erase(*proxy);
    return ustl::move(proxy->m_pager_ref);
}
} // namespace ours::object
