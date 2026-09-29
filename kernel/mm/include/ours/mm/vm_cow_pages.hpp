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

    /// A node of |size| bytes hanging under |parent|, in the parent's tree.
    ///
    /// The child's byte 0 is the parent's byte |parent_offset|, and it sees the
    /// parent only in its first |parent_limit| bytes: what lies past that
    /// reads as zero however large the child is, which is how a segment's
    /// `.bss`, or the tail of the last page of its file, comes out zero. The
    /// limit given is clamped, as Zircon's ClampedLimit() does, to what the
    /// child can hold and to what the parent has past |parent_offset|, so a
    /// caller may pass a length it has not checked. All three must be page
    /// aligned.
    ///
    /// |hidden| makes a node that holds pages for its children and is never a
    /// VMO of its own; see is_hidden().
    ///
    /// The child keeps |parent| alive and takes over its lock (lock()), which
    /// is what lets a walk from the child up through its ancestors run under
    /// one mutex. Nothing links the child into the parent yet: the parent does
    /// not know its children, because nothing walks down.
    static auto create_child(ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                             usize size, bool hidden, ustl::Arc<VmCowPages> *out) -> Status;

    /// Whether this is a node that only holds pages its children share.
    ///
    /// A hidden node is never written: what it holds is content that at least
    /// two nodes below it can see, and a write would be seen by all of them.
    /// Its pages leave it by being copied or moved out into a node that
    /// writes, never by a write to it, so an owned page is refused here and its
    /// pages are never reported writable. Zircon: VmCowPages::is_hidden().
    ///
    /// Fixed when the node is made, so it needs no lock.
    [[nodiscard]]
    FORCE_INLINE
    auto is_hidden() const -> bool {
        return m_hidden;
    }

    /// How many pages this node itself holds, not counting any it can see
    /// through a parent.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_pages_locked() const -> usize {
        return m_pages.size();
    }

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
    /// cow_pages_->lock().
    ///
    /// The mutex is not this object's own but its tree's (HierarchyState), so
    /// that when nodes come to hang under one another one lock covers the
    /// walk from a leaf to the pages it reads. A VmCowPages that has no
    /// relatives is a tree of one, and behaves as before.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_lock;
    }

    FORCE_INLINE
    auto size_locked() const -> usize {
        return m_size;
    }

  private:
    /// The lock of one whole tree of VmCowPages, held by every node in it.
    ///
    /// A node cannot own the tree's mutex, because any node can die first:
    /// the file VMO a launcher clones is closed while its clones run, and the
    /// clones still have to lock the same mutex as each other and as whatever
    /// remains of the tree above them. So the mutex is an object of its own,
    /// kept alive by a reference from each node, and it goes when the last
    /// node does. The same shape as PeerHolder for the two ends of a channel.
    ///
    /// Zircon counterpart: VmHierarchyState, which every VmCowPages held as
    /// `hierarchy_state_ptr_` until commit 2dad682fe14 replaced it with one
    /// lock per node and a lock order (the comment at `VmCowPages::lock()` in
    /// vm_cow_pages.h still describes the old scheme). One mutex per tree is
    /// decision 3 of the vmo-snapshot-child design: the simplest thing that is
    /// correct. Every process that clones one program contends on it, and the
    /// benchmark that says whether that matters comes after the feature works.
    class HierarchyState final
        : public ustl::RefCounted<HierarchyState, ustl::sync::ThreadSafePolicy> {
      public:
        HierarchyState() = default;

        [[nodiscard]]
        auto lock() -> sync::Mutex & {
            return m_lock;
        }

      private:
        sync::Mutex m_lock;
    };

    /// RefCounted::destroy() calls ~T() when the last reference goes; the
    /// destructor stays private otherwise, as MemCache and Dispatcher do.
    friend class ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy>;

    /// ObjectCache::Instance placement-news and destroys T, so it needs the
    /// constructor that stays private to everyone else.
    template <typename, typename...>
    friend class ObjectCache::Instance;

    /// |hierarchy| is the tree this node belongs to: a new one for a node with
    /// no relatives, the parent's for one that hangs under it. It is a
    /// parameter, not made here, because making one can fail and a constructor
    /// cannot say so. |parent| is empty for a node that hangs under nothing;
    /// create_child() clamps |parent_limit| before it gets here.
    VmCowPages(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy,
               ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit, bool hidden);

    ~VmCowPages();

    auto alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status;

    /// When no page sources exists, it will be used in frame allocation request.
    Gaf m_gaf;
    /// Page map that operates within the virtual memory range of the VMO, from [0, N).
    VmPageMap m_pages;
    ustl::Arc<PageSource> m_page_source;
    usize m_size;

    /// The node this one hangs under, or empty. It is a strong reference, so a
    /// parent lives as long as any node below it, whichever handle to it the
    /// user closes first; a parent cannot point back the same way without a
    /// cycle, which is one more reason it keeps no list of children.
    ustl::Arc<VmCowPages> m_parent;
    /// Where this node's byte 0 is in the parent, and how many of its bytes
    /// from there on the parent's content covers: this node sees the parent
    /// only in [0, m_parent_limit). Past that it reads as zero. Zircon:
    /// parent_offset_ and parent_limit_. All fixed at creation, so they are
    /// read without the lock.
    u64 const m_parent_offset;
    u64 const m_parent_limit;
    bool const m_hidden;

    /// Declared before m_lock, which is initialised from it, and destroyed
    /// after it: the reference is what keeps the mutex alive.
    ustl::Arc<HierarchyState> m_hierarchy;

    // Not TA_GUARDED over m_pages: the pages are walked by Cursor, a separate
    // object that the analysis cannot tie back to this lock. The entry points
    // that make a cursor or commit require it instead.
    //
    // A reference to the tree's mutex, as VmObject::m_lock is to this one, so
    // the annotations keep naming a member.
    sync::Mutex &m_lock;
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
    /// The page an ancestor of the target holds for the offset the cursor sits
    /// on, or nullptr when the walk up reaches the top, or a node's limit,
    /// without finding one; what lies past nothing reads as zero. The nearest
    /// ancestor that has a page wins, and the walk never looks at the target's
    /// own store: the callers have already done that.
    ///
    /// Zircon: VmCowPages::FindPageContentLocked(), which loops
    /// `while (offset < cur.parent_limit_)` in the same way, adding the
    /// parent offset at each hop.
    [[nodiscard]]
    auto find_ancestor_page() const -> VmPage *;

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