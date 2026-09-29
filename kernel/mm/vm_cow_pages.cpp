#include <ours/mm/object_cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vmm.hpp>

#include <algorithm>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
using VmCowPagesCache = ObjectCache::Instance<VmCowPages>;

VmCowPages::VmCowPages(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy,
                       ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                       bool hidden)
    // Initializer order follows the member declaration order in the header:
    // the reference to the mutex can only be taken from a hierarchy that is
    // already held.
    : m_gaf(gaf)
    , m_size(size)
    , m_parent(ustl::move(parent))
    , m_parent_offset(parent_offset)
    , m_parent_limit(parent_limit)
    , m_hidden(hidden)
    , m_hierarchy(ustl::move(hierarchy))
    , m_lock(m_hierarchy->lock()) {}

/// Returns every page this object owns.
///
/// It used to be `= default`, which meant a VMO's pages were never given back:
/// VmPageMap frees only its tree nodes, decommit() is Unimplemented, and the
/// single free_frames() on this path is the one that undoes a failed insert.
/// Nothing noticed while the only VMOs were kernel stacks and the userboot
/// image, because neither is ever destroyed -- but it makes any allocator
/// built on VMOs leak every byte it frees, which is exactly what heap::vfree()
/// would have done.
///
/// Zircon counterpart: VmCowPages::~VmCowPages(), which calls
/// page_list_.RemoveAllPages() and frees the batch.
VmCowPages::~VmCowPages() {
    m_pages.drain_owned([](VmPage *const page) { free_frames(page, 0); });
}

auto VmCowPages::create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status {
    // A tree of one: this node is its own root, so it starts its own lock.
    auto hierarchy = ustl::Arc<HierarchyState>::adopt(new (GAF_KERNEL) HierarchyState());
    if (!hierarchy) {
        return Status::OutOfMem;
    }

    auto cow_pages = VmCowPagesCache::get(GAF_KERNEL, gaf, size, ustl::move(hierarchy),
                                          ustl::Arc<VmCowPages>{}, u64(0), u64(0), false);
    if (!cow_pages) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(cow_pages);

    return Status::Ok;
}

auto VmCowPages::create_child(ustl::Arc<VmCowPages> parent, u64 const parent_offset,
                              u64 const parent_limit, usize const size, bool const hidden,
                              ustl::Arc<VmCowPages> *out) -> Status {
    DEBUG_ASSERT(parent);

    if (!ustl::mm::is_aligned(parent_offset, PAGE_SIZE) ||
        !ustl::mm::is_aligned(parent_limit, PAGE_SIZE) || !ustl::mm::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    // Clamped, so that what a child claims to see of its parent is never more
    // than the child can hold nor more than the parent has from |parent_offset|
    // on. Read without the lock: a paged object's size is fixed when it is made
    // (nothing can resize one), and this reads nothing else of the parent.
    // Zircon: ClampedLimit(), vm_cow_pages.cc.
    u64 limit = std::min<u64>(parent_limit, size);
    if (parent_offset >= parent->m_size) {
        limit = 0;
    } else {
        limit = std::min<u64>(limit, parent->m_size - parent_offset);
    }

    // Copied out before |parent| is handed on: the node that takes it does not
    // exist yet, and what it borrows -- the flags and the lock -- is the
    // parent's.
    auto const gaf = parent->m_gaf;
    auto hierarchy = parent->m_hierarchy;

    auto cow_pages = VmCowPagesCache::get(GAF_KERNEL, gaf, size, ustl::move(hierarchy),
                                          ustl::move(parent), parent_offset, limit, hidden);
    if (!cow_pages) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(cow_pages);

    return Status::Ok;
}

auto VmCowPages::make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status> {
    return ustl::ok(Cursor(this, offset, size));
}

auto VmCowPages::alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status {
    // Gaf::Zero unconditionally, on top of whatever the object was created
    // with. Every byte of a VMO that nobody has supplied reads as zero by
    // definition, and the buddy allocator hands back whatever the previous
    // owner left in the frame -- so without this the first read of a
    // freshly-faulted page returns another owner's data. None of the VMOs
    // built today ask for Gaf::Zero themselves (they all pass GAF_KERNEL,
    // which does not imply it), so the guarantee has to live here rather than
    // rest on callers remembering. A page source, once one exists, overwrites
    // the whole page anyway and should be allowed to skip the fill.
    auto frame = alloc_frames(m_gaf | Gaf::Zero, order);
    if (!frame) {
        return Status::OutOfMem;
    }

    // Fresh from the buddy allocator: declare the role, do not query it.
    frame->become<page::VmmBody>();
    *page = frame;
    return Status::Ok;
}

auto VmCowPages::commit_range_locked(VirtAddr offset, usize size, usize *nr_commited) -> Status {
    if (!size) {
        return Status::InvArgs;
    }

    if (offset + size > m_size) {
        return Status::InvArgs;
    }

    auto cursor = make_cursor(offset, size);
    if (!cursor) {
        return Status::InternalError;
    }

    auto status = Status::Ok;
    usize commited = 0;
    MultiPageRequest page_request;
    for (usize i = 0; i < size; i += PAGE_SIZE) {
        auto result = cursor->require_owned_page(1, &page_request);
        if (!result) {
            status = result.unwrap_err();
            DEBUG_ASSERT(status != Status::ShouldWait);
            break;
        }
        commited += 1;
    }

    if (nr_commited) {
        *nr_commited = commited;
    }

    return status;
}

/// The followings are in class VmCowPages::Cursor.

VmCowPages::Cursor::Cursor(VmCowPages *target, VirtAddr offset, usize size)
    // Initializer order follows the member declaration order in the header.
    : m_target(target), m_offset(offset), m_end(offset + size) {}

auto VmCowPages::Cursor::require_page(bool will_write, usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    return will_write ? require_owned_page(nr_pages, request)
                      : require_read_page(nr_pages, request);
}

auto VmCowPages::Cursor::require_owned_page(usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    DEBUG_ASSERT(m_offset < m_end, "the cursor stepped past the range it was made for");

    // Replace page refernece with a real page.
    if (is_type_reference()) {
        auto const status = convert_reference_to_page(request->get_anonymous());
        if (Status::Ok != status) {
            return ktl::err(status);
        }
    }

    // A hidden node is never written, so it has nothing to give a writer, not
    // even a page it holds: that page is shared with every node below it.
    // Asked before the slot, so that the answer does not depend on whether the
    // slot happens to be full.
    if (m_target->m_hidden) {
        return ktl::err(Status::BadState);
    }

    // The slot first, before any thought of allocating. This ordering is the
    // whole point of the class: a fault on an offset this VMO has already
    // populated must resolve to the page that is already there, or two
    // mappings of the same VMO see different memory and a re-fault silently
    // discards what the first fault wrote.
    //
    // A page found in this object's own store is its own, so it may be written.
    if (auto *page = m_target->m_pages.get_page(index())) {
        advance();
        return ktl::ok(RequireResult{.page = page, .writable = true});
    }

    // Nothing of its own here, but a node with a parent may see content there
    // that a write has to copy first: the page is the parent's to keep, and
    // this node gets its own copy. Writing that is the next step; until then a
    // write to such a node is refused rather than answered with an allocation
    // of a zeroed page, which would silently discard what the parent holds.
    if (m_target->m_parent) {
        return ktl::err(Status::Unimplemented);
    }

    // Nothing here. Who answers that depends on whether anyone can:
    //
    //   - With a page source, the content lives somewhere this kernel cannot
    //     reach synchronously, so the only honest answer is to raise a request
    //     and tell the caller to wait. That path is not built (see repair item
    //     3) and no VMO carries a source yet, so it reports Unimplemented
    //     rather than pretending.
    //   - Anonymous -- every VMO today -- has no one to ask. "Nothing here"
    //     means zero, and zero is something this kernel can produce on the
    //     spot. So the answer is an allocation, synchronous, never ShouldWait.
    if (m_target->m_page_source) {
        return ktl::err(create_read_request(nr_pages, request->get_read()));
    }

    VmPage *page = nullptr;
    if (auto const status = m_target->alloc_pages(0, &page, nullptr); Status::Ok != status) {
        return ktl::err(status);
    }

    // Recorded before it is handed out, and the page given back if recording
    // fails. An unrecorded page is worse than no page: the caller maps it, the
    // VMO does not know it exists, and the next fault on this same offset
    // allocates a second one over the top of it.
    if (auto const status = m_target->m_pages.insert_page(index(), page, VmPageMap::Tag::Owned);
        Status::Ok != status) {
        free_frames(page, 0);
        return ktl::err(status);
    }

    advance();
    return ktl::ok(RequireResult{.page = page, .writable = true});
}

auto VmCowPages::Cursor::require_read_page(usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    DEBUG_ASSERT(m_offset < m_end, "the cursor stepped past the range it was made for");

    // Its own page: writable, unless this is a hidden node, whose pages are
    // shared with every node below it and are never written through it.
    if (auto *page = m_target->m_pages.get_page(index())) {
        advance();
        return ktl::ok(RequireResult{.page = page, .writable = !m_target->m_hidden});
    }

    // A node that hangs under another, and a hidden one, answers a read of
    // what it does not hold from above, and allocates nothing to do so: the
    // nearest ancestor's page, on loan and read-only, or the shared zero page
    // when the walk finds none (which includes an offset past the node's limit,
    // where the parent's content does not reach). Zircon: RequireReadPage()
    // returning a parent page, or vm_get_zero_page(), with writable = false.
    if (m_target->m_parent || m_target->m_hidden) {
        auto *const page = find_ancestor_page();
        advance();
        if (page) {
            return ktl::ok(RequireResult{.page = page, .writable = false});
        }
        return ktl::ok(RequireResult{.page = get_zero_page(), .writable = false});
    }

    // A read of an offset nobody has populated reads zero. Zircon answers it
    // with a marker (VmPageOrMarker::Zero) in RequireReadPage, so a read-only
    // walk over a sparse VMO costs no memory at all and only a later write
    // turns the marker into a page.
    //
    // A node with no relatives still commits a real zeroed page and hands that
    // back, though ZERO_PAGE and the writable bit now exist: identical in what
    // the reader observes, more expensive in footprint, and the only answer
    // that is safe until the fault path maps by |writable|. Handing the shared
    // page to a caller that maps it as it maps every page would let a write
    // through, and every reader of every hole would see every writer.
    return require_owned_page(nr_pages, request);
}

auto VmCowPages::Cursor::find_ancestor_page() const -> VmPage * {
    auto const *node = m_target;
    auto offset = m_offset;

    // |offset| is in the coordinates of |node|; the limit and the parent offset
    // are that node's own, so the offset is compared with the limit first and
    // only then moved into the parent's coordinates.
    while (node->m_parent && offset < node->m_parent_limit) {
        offset += node->m_parent_offset;
        node = node->m_parent.as_ptr();
        if (auto *const page = node->m_pages.get_page(PgOff(offset >> PAGE_SHIFT))) {
            return page;
        }
    }

    return nullptr;
}

/// What the cursor is currently sitting on.
///
/// VmPageMap stores pages and nothing else: it has no marker for a zero slot
/// and no reference entry for a page owned by a parent VMO, because a parent's
/// page is found by walking up (find_ancestor_page()), not recorded below. So a
/// slot is either populated with a plain page or empty, and these answer
/// accordingly rather than guessing. When zero markers arrive, these three are
/// where the slot's kind gets decoded.
auto VmCowPages::Cursor::is_type_page() const -> bool {
    return m_target->m_pages.get_page(index()) != nullptr;
}

auto VmCowPages::Cursor::is_type_zero() const -> bool {
    return false;
}

auto VmCowPages::Cursor::is_type_reference() const -> bool {
    return false;
}

auto VmCowPages::Cursor::convert_reference_to_page(AnonymousPageRequest *request) -> Status {
    // Unreachable while is_type_reference() is always false.
    return Status::Unimplemented;
}

auto VmCowPages::Cursor::create_read_request(usize nr_pages, PageRequest *page_request) -> Status {
    return Status::Unimplemented;
}

auto VmCowPages::Cursor::create_dirty_request(usize nr_pages, PageRequest *page_request) -> Status {
    return Status::Unimplemented;
}

INIT_CODE
static auto init_vm_cow_pages_cache() -> void {
    auto const status = VmCowPagesCache::init("vm-cow-pages-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmCowPages");
    }
    log::trace("VmCowPagesCache has been created");
}
// Heap, not PlatformEarly: this creates a MemCache, so it needs the object
// caches to be up.
GKTL_INIT_HOOK(VmCowPagesCacheInit, init_vm_cow_pages_cache, gktl::InitLevel::Heap);

} // namespace ours::mm