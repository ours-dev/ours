#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <algorithm>

#include <cstring>

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

/// One walk over the pages [offset, offset + size) touches, handing |copy| the
/// physmap address of each and the slice of the caller's buffer that belongs
/// to it.
///
/// Zircon counterpart: VmObjectPaged::ReadWriteInternal, with the same shape
/// -- range check, a lookup cursor over the whole page range, RequirePage per
/// page, copy through the physmap -- minus everything that exists for a user
/// buffer: dropping the lock around a copy that may fault, the partial-length
/// return, the soft fault retry. read() and write() take kernel buffers only
/// (see VmObject::read()), so the copy cannot fault and runs under m_mutex.
/// That is a sleeping lock, not a spin lock, so a long copy holds up other
/// users of this VMO but never a CPU with interrupts off.
template <typename Copy>
auto VmObjectPaged::read_write(VirtAddr const offset, usize const size, bool const will_write,
                               Copy const &copy) -> Status {
    m_canary.verify<Self>();

    if (!size) {
        return Status::Ok;
    }

    Guard guard{m_mutex};
    // Checked before anything is copied, and written so that it cannot wrap,
    // as commit_range_internal() is. A range that runs off the end copies
    // nothing rather than a prefix: no caller here wants a short read, and
    // Zircon answers ZX_ERR_OUT_OF_RANGE the same way unless the caller asks
    // for TrimLength.
    auto const total = m_cow_pages->size_locked();
    if (size > total || offset > total - size) {
        return Status::OutOfRange;
    }

    // The cursor serves whole pages, so it is made for the page-aligned span
    // that covers the byte range; the copy below cuts each page back down to
    // the bytes that were asked for.
    auto const first_page = align_down(offset, PAGE_SIZE);
    auto const end_page = align_up(offset + size, PAGE_SIZE);
    auto cursor = m_cow_pages->make_cursor(first_page, end_page - first_page);
    if (!cursor) {
        return cursor.unwrap_err();
    }

    // The request only matters when a page source has to be asked, which no
    // VMO has yet; the cursor is handed one anyway, as the fault path does,
    // because that is its contract.
    MultiPageRequest request;
    usize done = 0;
    while (done < size) {
        auto const at = offset + done;
        auto const in_page = at - align_down(at, PAGE_SIZE);
        auto const chunk = std::min<usize>(size - done, PAGE_SIZE - in_page);

        // A reader is content with whatever is there and a writer needs a page
        // of its own -- the same choice the fault path makes. For an anonymous
        // VMO both commit a zeroed page where there was none, so a read of a
        // never-written page reads zero (see VmCowPages::Cursor).
        auto page = cursor->require_page(will_write, 1, &request);
        if (!page) {
            return page.unwrap_err();
        }

        auto *const base = PhysMap::phys_to_virt<u8>(frame_to_phys(*page));
        copy(base + in_page, done, chunk);
        done += chunk;
    }

    return Status::Ok;
}

auto VmObjectPaged::read(void *const out, VirtAddr const offset, usize const size) -> Status {
    auto *const dst = static_cast<u8 *>(out);
    return read_write(offset, size, false,
                      [dst](u8 const *const page, usize const done, usize const chunk) {
                          std::memcpy(dst + done, page, chunk);
                      });
}

auto VmObjectPaged::write(void const *const in, VirtAddr const offset, usize const size) -> Status {
    auto const *const src = static_cast<u8 const *>(in);
    return read_write(offset, size, true,
                      [src](u8 *const page, usize const done, usize const chunk) {
                          std::memcpy(page, src + done, chunk);
                      });
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