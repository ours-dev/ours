/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_COW_PAGES_HPP
#define OURS_MM_VM_COW_PAGES_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_page_map.hpp>
#include <ours/sync/mutex.hpp>

#include <ustl/rc.hpp>

namespace ours::mm {
/// A group of the copy on write page.
class VmCowPages: public ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy> {
    using Self = VmCowPages;

  public:
    class Cursor;

    static auto create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status;

    static auto with_source(ustl::Arc<PageSource> src, Gaf gaf, usize size,
                            ustl::Arc<VmCowPages> *out) -> Status;

    TA_REQUIRES(m_lock)
    auto commit_range_locked(VirtAddr offset, usize size, usize *nr_commited) -> Status;

    TA_REQUIRES(m_lock)
    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status>;

    /// The lock of every VmObjectPaged over these pages: over the page tree,
    /// and -- through VmObject::lock(), which is this same mutex for a paged
    /// VMO -- over each such object's list of mappings.
    ///
    /// It lives here, with the pages, because more than one object can sit on
    /// one set of pages: a slice child shares its parent's VmCowPages. Were
    /// the lock in the object, parent and child would each lock the same page
    /// tree with a different mutex, which is no lock at all. Zircon keeps it
    /// with the pages for the same reason: VmObjectPaged::lock() answers
    /// cow_pages_->lock(), shared by the whole hierarchy.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_lock;
    }

    FORCE_INLINE
    auto size_locked() const -> usize {
        return m_size;
    }

  private:
    /// RefCounted::destroy() calls ~T() when the last reference goes; the
    /// destructor stays private otherwise, as MemCache and Dispatcher do.
    friend class ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy>;

    /// ObjectCache::Instance placement-news and destroys T, so it needs the
    /// constructor that stays private to everyone else.
    template <typename, typename...>
    friend class ObjectCache::Instance;

    VmCowPages(Gaf gaf, usize size);

    ~VmCowPages();

    auto alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status;

    /// When no page sources exists, it will be used in frame allocation request.
    Gaf m_gaf;
    /// Page map that operates within the virtual memory range of the VMO, from [0, N).
    VmPageMap m_pages;
    ustl::Arc<PageSource> m_page_source;
    usize m_size;

    // Not TA_GUARDED over m_pages: the pages are walked by Cursor, a separate
    // object that the analysis cannot tie back to this lock. The entry points
    // that make a cursor or commit require it instead.
    sync::Mutex m_lock;
};

/// A walk over one contiguous byte range of a VmCowPages, handing out the page
/// for each offset in turn.
///
/// The cursor holds a position because its callers do not: map_paged() and
/// commit_range_locked() both loop `require_*_page(1, ...)` once per page and
/// never say which page they mean, so the cursor is the only thing that can
/// know. It used to discard the offset it was constructed with, which made
/// every iteration ask for the same nothing and get a fresh allocation back.
///
/// Modelled on Zircon's VmCowPages::LookupCursor, which likewise carries
/// offset_/end_offset_ and steps them in RequireOwnedPage / RequireReadPage.
class VmCowPages::Cursor {
    using Self = VmCowPages::Cursor;

  public:
    /// What a lookup hands back: the page, and whether the caller may write it.
    ///
    /// The two are separate because the second is not a property of the page.
    /// The same physical page can be the writer's own in one object and content
    /// on loan from an ancestor in another, and only the cursor, which knows
    /// which object it found the page in, can say which. A caller that maps the
    /// page must map it read-only when |writable| is false, even into a mapping
    /// whose own permission includes Write: the write then faults, asks for an
    /// owned page, and gets the copy instead of the ancestor's page.
    ///
    /// Zircon counterpart: LookupCursor::RequireResult{page, writable}
    /// (vm_cow_pages.h), which PageAsResultNoIncrement() fills in.
    struct RequireResult {
        VmPage *page;
        bool writable;
    };

    Cursor(VmCowPages *owner, VirtAddr offset, usize size);
    ~Cursor() = default;

    /// Hand back the page for the offset the cursor sits on, then step past it.
    ///
    /// |will_write| picks the discipline: a writer needs a page this object
    /// owns, a reader would be content with shared content. Zircon spells the
    /// same dispatch as LookupCursor::RequirePage(bool write, ...); ours took
    /// no such flag, which is one reason it could never be defined.
    ///
    /// A request to write always comes back |writable|; a request to read may
    /// not, and every caller has to look, because it decides how the page is
    /// mapped. Until an object can have an ancestor every page an object hands
    /// out is its own, so every answer is |writable| today.
    ///
    /// |nr_pages| is how far ahead the caller would accept pages for, and is
    /// only consulted when a page source has to be asked; |request| is the
    /// caller's stack-allocated waiting token, threaded down rather than
    /// allocated here because the fault path must not enter the allocator and
    /// risk recursing into reclaim and faulting again.
    auto require_page(bool will_write, usize nr_pages, MultiPageRequest *request)
        -> ktl::Result<RequireResult>;
    auto require_read_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<RequireResult>;
    auto require_owned_page(usize nr_pages, MultiPageRequest *request)
        -> ktl::Result<RequireResult>;

    auto create_read_request(usize nr_pages, PageRequest *request) -> Status;
    auto create_dirty_request(usize nr_pages, PageRequest *request) -> Status;

  private:
    auto convert_reference_to_page(AnonymousPageRequest *request) -> Status;

    [[nodiscard]]
    auto is_type_reference() const -> bool;
    [[nodiscard]]
    auto is_type_page() const -> bool;
    [[nodiscard]]
    auto is_type_zero() const -> bool;

    /// The page index the cursor currently sits on.
    [[nodiscard]]
    FORCE_INLINE
    auto index() const -> PgOff {
        return PgOff(m_offset >> PAGE_SHIFT);
    }

    /// Step past the page just handed out. Separate from the lookups so all
    /// three exits -- hit, fresh allocation, borrowed page -- advance in one
    /// place and cannot disagree.
    FORCE_INLINE
    auto advance() -> void {
        m_offset += PAGE_SIZE;
    }

    VmCowPages *m_target;
    /// Byte offsets into the owning VMO, not addresses in any address space:
    /// |m_offset| is the slot being served and |m_end| is one past the last
    /// byte this cursor was made for. Byte rather than page offsets to match
    /// the rest of the VmObject interface, which is byte-addressed because a
    /// physical VMO need not begin on a page boundary.
    VirtAddr m_offset;
    VirtAddr m_end;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_COW_PAGES_HPP