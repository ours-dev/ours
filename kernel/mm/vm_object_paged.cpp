#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

using ustl::mm::align_down;
using ustl::mm::align_up;

namespace ours::mm {
using VmoPagedCache = ObjectCache::Instance<VmObjectPaged>;

VmObjectPaged::VmObjectPaged(VmoFlags vmof, ustl::Arc<VmCowPages> cowpages)
    : Base(gktl::magic<Self>(), Type::Paged, vmof), m_cow_pages(ustl::move(cowpages)) {}

auto VmObjectPaged::create(Gaf gaf, usize size, VmoFlags vmof, ustl::Arc<VmObjectPaged> *out)
    -> Status {
    // Check vmof
    ustl::Arc<VmCowPages> cow_pages;
    auto status = VmCowPages::create(gaf, size, &cow_pages);
    if (Status::Ok != status) {
        return status;
    }

    auto vmo = VmoPagedCache::get(GAF_KERNEL, vmof, ustl::move(cow_pages));
    if (!vmo) {
        return Status::OutOfMem;
    }
    // Adopted before the commit, not after it. If the commit fails part way,
    // dropping this reference is what gives back the pages it did get
    // (~VmCowPages frees every page it owns); adopted afterwards, a failed
    // commit would leave nobody holding the object or its pages.
    auto vmo_ref = ustl::Arc<Self>::adopt(vmo);

    // A non-lazy VMO has every page it will ever need committed here, so that
    // no access to it can fault. That is the whole promise of the flag, and it
    // is why the status matters: a kernel stack is Pinned, and a stack page
    // that is missing turns the first push that reaches it into a page fault
    // on a kernel address -- which page_fault() refuses and the kernel dies
    // of. The status used to be thrown away, and create() answered Ok for a
    // VMO with holes in it.
    //
    // An empty VMO has nothing to commit and is skipped, not handed to
    // commit_range(): a Pinned commit of zero bytes is InvArgs there, as it
    // is in Zircon's CommitRangeInternal, because pinning nothing is a caller
    // error. VmArea::reserve() makes exactly such a VMO for every reservation
    // it records -- and while the commit status was ignored, nobody saw that
    // its commit had always failed.
    if (!(vmof & VmoFlags::Lazy) && size != 0) {
        CommitOptions options{};
        if (!!(vmof & VmoFlags::Pinned)) {
            options |= CommitOptions::Pin;
        }

        auto const commit_status = vmo_ref->commit_range(0, size, options);
        if (Status::Ok != commit_status) {
            return commit_status;
        }
    }
    *out = ustl::move(vmo_ref);

    return Status::Ok;
}

FORCE_INLINE
auto VmObjectPaged::commit_range_internal(VirtAddr offset, usize size, CommitOptions option)
    -> Status {
    if (!!(option & CommitOptions::Pin)) {
        if (!size) {
            return Status::InvArgs;
        }
    }

    if (!size) {
        return Status::Ok;
    }

    Guard guard{m_mutex};
    // The whole range, offset included, and written so that it cannot wrap:
    // this used to compare |size| alone against the object's size, so a range
    // starting anywhere but 0 was admitted however far it ran past the end.
    auto const total = m_cow_pages->size_locked();
    if (size > total || offset > total - size) {
        return Status::OutOfRange;
    }

    // One call commits the whole range. This used to sit in a loop that called
    // it once per page -- each time for the entire range, so a VMO of n pages
    // cost n^2 page lookups, and a Pinned kernel stack paid it on every thread
    // created.
    //
    // The loop was a half-copy of Zircon's VmObjectPaged::CommitRangeInternal,
    // whose loop exists for one reason: CommitRangeLocked may stop early with
    // ZX_ERR_SHOULD_WAIT when a page source has to supply pages, and the loop
    // waits on the page request with the lock dropped, advances offset by the
    // length committed so far, and tries again. Without the advance the loop
    // only repeats itself. And there is nothing here to wait for: no VMO has a
    // page source yet, and commit_range_locked() asserts that it never answers
    // ShouldWait. When the first pager-backed VMO arrives, bring back Zircon's
    // loop whole -- the wait, the advance, the retry -- not the shape of it.
    usize nr_committed = 0;
    return m_cow_pages->commit_range_locked(offset, size, &nr_committed);
}

auto VmObjectPaged::commit_range(VirtAddr offset, usize size, CommitOptions option) -> Status {
    m_canary.verify<Self>();

    log::trace("VMO Commit Action: [ofs: {:X}, size: {:X}]", offset, size);
    return commit_range_internal(offset, size, option);
}

auto VmObjectPaged::decommit(VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::take_pages(VirtAddr offset, usize size, VmPage::List *pages) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::supply_pages(VirtAddr offset, usize size, VmPage::List *pages,
                                 SupplyOption options) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::read(void *out, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

auto VmObjectPaged::write(void *in, VirtAddr offset, usize size) -> Status {
    m_canary.verify<Self>();

    return Status::Unimplemented;
}

INIT_CODE
static auto init_vmo_paged_cache() -> void {
    auto const status = VmoPagedCache::init("vmo-paged-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmArea");
    }
    log::trace("VmoPagedCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmoPagedCacheInit, init_vmo_paged_cache, gktl::InitLevel::Heap);

} // namespace ours::mm