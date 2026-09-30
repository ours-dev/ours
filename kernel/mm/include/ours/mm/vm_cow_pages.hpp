/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_COW_PAGES_HPP
#define OURS_MM_VM_COW_PAGES_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_page_map.hpp>
#include <ours/sync/mutex.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
/// A group of the copy on write page.
class VmCowPages: public ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy> {
    using Self = VmCowPages;

  public:
    class Cursor;

    /// What kind of node this is, fixed when it is made. A group of flags and
    /// not a bool for each, because a node will have more of them, and each new
    /// bool would have to be threaded through every factory by hand.
    enum class Flags : u8 {
        /// No flag: an ordinary node.
        None = 0,

        /// A node that holds pages for the nodes below it and is never a VMO of
        /// its own. See is_hidden().
        Hidden = BIT(0),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

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
    /// |flags| is what kind of node it is; Flags::Hidden makes one that holds
    /// pages for its children and is never a VMO of its own, see is_hidden().
    ///
    /// The child keeps |parent| alive and takes over its lock (lock()), which
    /// is what lets a walk from the child up through its ancestors run under
    /// one mutex. It is put on the parent's list of children, and takes itself
    /// off when it dies.
    ///
    /// Nothing but a test makes a node this way. The tree the rest of the
    /// kernel builds has a parent above a node only because a clone put it
    /// there, and it is a hidden node; see create_clone_locked().
    static auto create_child(ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                             usize size, Flags flags, ustl::Arc<VmCowPages> *out) -> Status;

    /// A copy-on-write clone of [offset, offset + size) of this node: a new
    /// node that sees what this one sees there, and neither ever sees the
    /// other's later writes.
    ///
    /// Nothing is copied. If this node holds pages of its own in the range they
    /// move, all of this node's pages together, into a new hidden node put
    /// between this node and its parent, and this node and the clone hang under
    /// it; every page the clone can see is then seen by one more node, which its
    /// share count records. If this node holds nothing in the range and hangs
    /// under a hidden node already, the clone hangs under that one, which is why
    /// a program started many times makes one hidden node with many leaves, not
    /// a chain.
    ///
    /// The clone sees this node's bytes only up to the end of this node: past it
    /// reads as zero however large |size| is. |offset| and |size| must be page
    /// aligned, and a hidden node cannot be cloned: BadState.
    ///
    /// Two things this cannot check, because a VmCowPages does not know the
    /// objects over it, and the caller must have seen to. Nothing may map this
    /// node's pages writable while it is cloned: the pages become shared, and a
    /// writable mapping would write to all of their viewers. And none of them
    /// may be pinned. Zircon refuses both itself (RemoveWrite on the mappings,
    /// BadState for a pinned page); the VmObject that calls this has to.
    ///
    /// The lock is taken by the caller, who has the answer to both anyway.
    /// Zircon: VmCowPages::CreateCloneLocked(), bidirectional path.
    TA_REQUIRES(m_lock)
    auto create_clone_locked(u64 offset, usize size, ustl::Arc<VmCowPages> *out) -> Status;

    /// The node this one hangs under, or null. A raw pointer for the life of
    /// the lock: it is only valid while the tree does not change.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto parent_locked() const -> VmCowPages const * {
        return m_parent.as_ptr();
    }

    /// Whether this is a node that only holds pages its children share.
    ///
    /// A hidden node is never written: what it holds is content that at least
    /// two nodes below it can see, and a write would be seen by all of them.
    /// Its pages leave it by being copied or moved out into a node that
    /// writes, never by a write to it, so an owned page is refused here and its
    /// pages are never reported writable. Zircon: VmCowPages::is_hidden().
    ///
    /// Flags::Hidden, fixed when the node is made, so it needs no lock.
    [[nodiscard]]
    FORCE_INLINE
    auto is_hidden() const -> bool {
        return !!(m_flags & Flags::Hidden);
    }

    /// How many pages this node itself holds, not counting any it can see
    /// through a parent.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_pages_locked() const -> usize {
        return m_pages.size();
    }

    /// How many nodes hang directly under this one.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_children_locked() const -> usize {
        return m_children.size();
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
               ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit, Flags flags);

    ~VmCowPages();

    auto alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status;

    /// A page a node sees, with where it was found. |page| is null when nothing
    /// was: the walk reached the top, or a node's limit, and what lies past
    /// nothing reads as zero.
    struct FoundPage {
        VmPage *page;
        /// The node that holds it, this one or an ancestor, and its offset in
        /// that node's coordinates: what a write needs to take the page out.
        VmCowPages *owner;
        u64 offset;
    };

    /// The page this node sees at |offset|, an offset in its own coordinates:
    /// the one in its own store if it has one, and if not the nearest ancestor's.
    /// One walk for both, because they are one question -- what does this node
    /// see here -- and its answer is where the page was found: a page in the
    /// node's own store may be written, and a page found further up is shared
    /// content that a writer has to copy or take first.
    ///
    /// The lock is held by every caller. It is not annotated, for the reason
    /// m_pages is not guarded: the callers include Cursor, which the analysis
    /// cannot tie to the lock.
    ///
    /// Zircon: VmCowPages::FindPageContentLocked(), which loops
    /// `while (offset < cur.parent_limit_)`, looking in the node and then adding
    /// the parent offset to go up.
    [[nodiscard]]
    auto find_page(u64 offset) -> FoundPage;

    /// Gives this node a page of its own at |offset|, out of |found|, the page
    /// an ancestor shows there now, and answers it. The node may write the page
    /// it gets.
    ///
    /// A page held by a hidden node is shared with every node that sees it, and
    /// its share count says how many others do. Above zero, someone else still
    /// sees it, so this node gets a copy (a fork) and the count drops by one.
    /// At zero this node is the only one left, so it takes the page itself (a
    /// migrate): the hidden node lets go of it and this node holds it, and
    /// nothing is copied. Never worse than a copy. A page held by a node that is
    /// not hidden belongs to a node that can still write it, so it is only ever
    /// copied, and no count is touched.
    ///
    /// Everything that can fail is done before anything changes: the copy is
    /// allocated and put into this node's store first, and only then is the
    /// share count dropped or the page taken from the hidden node, neither of
    /// which can fail, so a failure leaves the tree as it was.
    ///
    /// All of it under the one lock the caller holds, from the count that decides
    /// to the count that changes. Two writers that each read "one other viewer"
    /// and then both fork would leave the count below zero and the page in the
    /// hidden node for good; a writer that reads zero while another still reads
    /// the page would take a page the other keeps seeing, and its writes would
    /// show there.
    ///
    /// Zircon: VmCowPages::CloneCowPageLocked(). What it also does, unmapping the
    /// writer's old read-only mapping on a fork, is not done here: this does not
    /// know the mappings.
    [[nodiscard]]
    auto cow_page(u64 offset, FoundPage const &found) -> ktl::Result<VmPage *>;

    /// Makes a node and adopts it. The one place a VmCowPages is allocated, so
    /// create(), create_child() and the clone do not each spell out the cache
    /// call and its OutOfMem. The node is not linked onto any list: that is
    /// the caller's, under the lock.
    static auto make_node(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy,
                          ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                          Flags flags, ustl::Arc<VmCowPages> *out) -> Status;

    /// When no page sources exists, it will be used in frame allocation request.
    Gaf m_gaf;
    /// Page map that operates within the virtual memory range of the VMO, from [0, N).
    VmPageMap m_pages;
    ustl::Arc<PageSource> m_page_source;
    usize m_size;

    /// The node this one hangs under, or empty. It is a strong reference, so a
    /// parent lives as long as any node below it, whichever handle to it the
    /// user closes first. The parent's list of children points back with a
    /// bare link and owns nothing, which is what keeps the two from being a
    /// cycle.
    ustl::Arc<VmCowPages> m_parent;
    /// Where this node's byte 0 is in the parent, and how many of its bytes
    /// from there on the parent's content covers: this node sees the parent
    /// only in [0, m_parent_limit). Past that it reads as zero. Zircon:
    /// parent_offset_ and parent_limit_.
    ///
    /// Set when the node is made and rewritten only when a clone puts a new
    /// node above this one, under the lock. The readers are cursors, which
    /// are made under it too.
    u64 m_parent_offset;
    u64 m_parent_limit;
    Flags const m_flags;

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

    /// How this node is strung on its parent's list of children. A safe link:
    /// the hook's destructor traps if the node is still linked, so a node that
    /// forgot to leave its parent's list cannot be freed quietly.
    ustl::collections::intrusive::ListMemberHook<> m_sibling_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_sibling_hook, SiblingOptions);
    USTL_DECLARE_LIST(Self, ChildList, SiblingOptions);

    /// The nodes that hang directly under this one, in no particular order.
    /// Not references: each child already holds this node, and a list
    /// that held the children would hold them for as long as this node lives.
    /// A child takes itself off in its destructor, under the lock.
    ///
    /// What reads it is what has to go down the tree: a clone that puts a new
    /// node above an old one has to replace it in its parent's list, and a
    /// node that dies has to find the sibling that is left. Both come later;
    /// until they do only creation and destruction touch it.
    ///
    /// Two rules for whoever walks it, both from the same fact: a node's
    /// destructor takes the tree's lock to leave this list.
    ///
    ///  - A node on the list has intact memory, but its reference count may
    ///    already be zero: once the last reference is gone the node stays here
    ///    until its destructor gets the lock. Read it and change its fields, but
    ///    take no reference to it (Arc::adopt, retain): that brings a node that
    ///    is being destroyed back to life, and it is then destroyed twice.
    ///    No walker needs one today. The merge of a dying node into its sibling
    ///    changes the sibling's fields and nothing else.
    ///  - Never release the last reference to a node while holding the lock: its
    ///    destructor takes the lock again and the thread waits for itself. Hand
    ///    the reference out and let the caller drop it after the lock, as Zircon's
    ///    MaybeDeadTransition() does.
    ///
    /// Under the tree's lock, like everything else here that more than one
    /// node can see.
    ChildList m_children TA_GUARDED(m_lock);
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
    /// A request to write always comes back |writable|: a node that sees the
    /// page only through an ancestor is given a page of its own first, by a
    /// copy or by taking the page (see cow_page()). A request to read may not:
    /// a node with an ancestor answers with the ancestor's page, on loan, or
    /// the shared zero page, and every caller has to look, because it decides
    /// how the page is mapped.
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