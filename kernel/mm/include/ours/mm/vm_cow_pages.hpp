/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_COW_PAGES_HPP
#define OURS_MM_VM_COW_PAGES_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_page_list.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/sync/mutex.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
class VmObjectPaged;

/// A group of the copy on write page.
class VmCowPages: public ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy> {
    using Self = VmCowPages;

  public:
    class Cursor;

    /// One VmObjectPaged over these pages, as the node knows it. A member of the
    /// object, which registers it with the node it is over and takes it off
    /// again when it dies, both under the tree's lock.
    ///
    /// The node has to know its objects because an operation on the pages has to
    /// reach every mapping of them, and the mappings belong to the objects: a
    /// slice shares its parent's pages and has a list of mappings of its own,
    /// so a single back pointer to one object would not do. This is composition
    /// and not a base class of the object, so that the node's header need not
    /// know the object's.
    class Attachment {
        using Self = Attachment;

      public:
        explicit Attachment(VmObjectPaged *owner) : m_owner(owner) {}
        USTL_NO_MOVEABLE_AND_COPYABLE(Attachment);

      private:
        friend class VmCowPages;

        VmObjectPaged *const m_owner;

        ustl::collections::intrusive::ListMemberHook<> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, Options);
        USTL_DECLARE_LIST(Self, List, Options);
    };

    /// What kind of node this is, fixed when it is made. A group of flags and
    /// not a bool for each, because a node will have more of them, and each new
    /// bool would have to be threaded through every factory by hand.
    enum class Flags : u8 {
        /// No flag: an ordinary node.
        None = 0,

        /// A node that holds pages for the nodes below it and is never a VMO of
        /// its own. See is_hidden().
        Hidden = 0_bit,

        /// A node with no relatives and no page source that fills a hole with
        /// the whole 2 MiB block it lies in, when the block fits in the node
        /// and holds nothing yet (fill_block_locked()); a hole where none fits,
        /// or when the allocator has no block, gets one page as any node's.
        LargeBlocks = 1_bit,

        /// The pages of a pinned VMO (VmoFlags::Pinned): filed on the pinned
        /// queue, never reclaimed. The VMO is what is pinned, but it is the
        /// node that files its pages, so the node is told.
        Pinned = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    /// A block: the 512 pages that one 2 MiB leaf of a page table maps, 2 MiB
    /// aligned and physically contiguous. What a node that takes blocks fills
    /// a hole with (fill_block_locked()), what find_block_locked() asks about,
    /// and what a mapping maps as one leaf.
    static constexpr usize BLOCK_SIZE = 2_mb;

    static auto create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status;

    /// A node with no relatives of the kind |flags| says; only LargeBlocks is
    /// one a node with no relatives can be.
    static auto create(Gaf gaf, usize size, Flags flags, ustl::Arc<VmCowPages> *out) -> Status;

    /// A node of |size| bytes with no relatives whose pages come from |src|:
    /// a page nobody has supplied is not zero but missing, and a lookup of it
    /// raises a request against the source (Cursor::create_read_request()).
    /// The node closes the source when it dies.
    ///
    /// Such a node has no children: create_child() and create_clone_locked()
    /// refuse it with NotSupported, because a lookup from a child stops at the
    /// root without asking the root's source.
    static auto with_source(ustl::Arc<PageSource> src, Gaf gaf, usize size,
                            ustl::Arc<VmCowPages> *out) -> Status;

    /// A node of |size| bytes hanging under |parent|, in the parent's tree.
    ///
    /// The child's byte 0 is the parent's byte |parent_offset|, and it sees the
    /// parent only in its first |parent_limit| bytes: what lies past that
    /// reads as zero however large the child is, which is how a segment's
    /// `.bss`, or the tail of the last page of its file, comes out zero. The
    /// limit given is clamped to what the child can hold and to what the
    /// parent has past |parent_offset|, so a caller may pass a length it has
    /// not checked. All three must be page aligned.
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
    /// Write is taken from every mapping of the pages that move, through the
    /// objects registered over this node (change_range_locked()), since the
    /// pages become shared and a writable mapping would write to all of their
    /// viewers. What this cannot check, because pinning is the object's and not
    /// the pages', is that none of them is pinned: the object that calls this
    /// has to check that, and answer BadState for a pinned page.
    ///
    /// The lock is taken by the caller, who has the answer to both anyway.
    ///
    /// |end| is where the clone stops seeing this node: an object that shows a
    /// window of the pages passes the end of its window, so that a clone of a
    /// slice does not see the parent's bytes past the slice. It is clamped to
    /// this node's own size, so a caller with no window passes ~0.
    TA_REQUIRES(m_lock)
    auto create_clone_locked(u64 offset, usize size, u64 end, ustl::Arc<VmCowPages> *out) -> Status;

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
    /// pages are never reported writable.
    ///
    /// Flags::Hidden, fixed when the node is made, so it needs no lock.
    [[nodiscard]]
    FORCE_INLINE
    auto is_hidden() const -> bool {
        return !!(m_flags & Flags::Hidden);
    }

    /// How many pages this node itself holds, not counting any it can see
    /// through a parent. A zero marker is no page.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_pages_locked() const -> usize {
        return m_pages.nr_pages();
    }

    /// How many of the pages of [offset, offset + len), in this node's own
    /// bytes, this node holds itself; a page its parent would supply is not
    /// counted, and neither is a zero marker.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_pages_in_locked(u64 const offset, u64 const len) const -> usize {
        if (len == 0) {
            return 0;
        }
        return m_pages.nr_pages_in(static_cast<PgOff>(offset >> PAGE_SHIFT),
                                   static_cast<PgOff>(((offset + len - 1) >> PAGE_SHIFT) + 1));
    }

    /// Registers and takes off an object over these pages; see Attachment.
    TA_REQUIRES(m_lock)
    auto attach_locked(Attachment &attachment) -> void {
        m_attachments.push_back(attachment);
    }

    TA_REQUIRES(m_lock)
    auto detach_locked(Attachment &attachment) -> void {
        if (attachment.m_hook.is_linked()) {
            m_attachments.erase(attachment);
        }
    }

    /// How many objects are over these pages.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_attached_locked() const -> usize {
        return m_attachments.size();
    }

    /// Makes |change| to the page tables of every mapping, of every object
    /// over these pages, of [offset, offset + len) of them (offsets in this
    /// node's bytes, which each object translates into its own): a clone takes
    /// Write away from the node it clones, since the pages are about to be
    /// shared, and take_pages() unmaps what it takes. See RangeChange.
    ///
    /// Every object over the node is reached, a slice included: a slice
    /// shares the pages and has mappings of its own (see Attachment).
    TA_REQUIRES(m_lock)
    auto change_range_locked(u64 offset, u64 len, RangeChange change) -> void;

    /// What reclaim_page() or dedup_zero_page() did.
    enum class ReclaimResult : u8 {
        /// The page is gone from the node, unmapped and freed. The next access
        /// to its offset asks the page source for it again, or, for a page
        /// that was all zero, finds a zero marker and reads zero.
        Evicted,
        /// The page is here and stays: it is not one the node can give back.
        Kept,
        /// The page is no longer the node's at that offset; nothing was done.
        Gone,
    };

    /// Gives |page|, the one a reclaimer found at |index| of this node, back
    /// to the system if it can be asked of the page source again: removes it
    /// from every mapping of every object over the node, takes it out of the
    /// node and frees it. Takes the node's lock itself.
    ///
    /// A reclaimer finds a page on a queue, with no lock of this node held, so
    /// everything it was told is checked again here: the page may have moved,
    /// gone, or been written since. Only a page the queues still file as
    /// reclaimable is evicted, and a page written is never that (see
    /// file_dirty_locked()).
    ///
    /// The last reference to this node must not be released by the caller
    /// while it holds a queue lock; the caller holds one of its own.
    auto reclaim_page(PgOff index, VmPage *page) -> ReclaimResult;

    /// Replaces |page|, the one a reclaimer found at |index| of this node
    /// among those committed in place of zero content, by a zero marker if it
    /// is all zero: removes it from every mapping, and frees it. A read then
    /// maps the shared zero page and a write commits a new one. Takes the
    /// node's lock itself, and checks what the reclaimer was told again, as
    /// reclaim_page() does.
    ///
    /// The page is looked at twice. The first look is cheap and may be wrong:
    /// it reads the page while a program may be writing it, and a page that is
    /// not zero is left in its mappings, writable, which is why this is not
    /// done after taking them away. A page that looks zero then loses Write in
    /// every mapping, which returns only once no CPU can write it through a
    /// cached entry, and is looked at again: now it cannot change, since a
    /// write faults and the fault needs the lock held here. Only then is it
    /// unmapped and freed.
    ///
    /// A page found not to be zero is filed anonymous, so that it is not the
    /// reclaimer's again. So is one this node cannot dedup: a hidden node's
    /// pages are shared with the nodes below it, and the mappings of those
    /// are not this node's to take Write from.
    auto dedup_zero_page(PgOff index, VmPage *page) -> ReclaimResult;

    /// How many nodes hang directly under this one.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto nr_children_locked() const -> usize {
        return m_children.size();
    }

    /// Makes every page of [offset, offset + size) the node's own, from the
    /// start, and says in |nr_commited| how many it got through. ShouldWait
    /// when a page source has to supply one first: the request for it is
    /// raised in |request|, which the caller waits on with this lock let go
    /// and then calls again from the page it stopped at.
    TA_REQUIRES(m_lock)
    auto commit_range_locked(VirtAddr offset, usize size, MultiPageRequest *request,
                             usize *nr_commited) -> Status;

    TA_REQUIRES(m_lock)
    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status>;

    /// One 2 MiB block as this node sees it: where its frames start, and
    /// whether the node may write them.
    struct FoundBlock {
        PhysAddr base;
        bool writable;
    };

    /// Whether the 512 pages this node sees in [offset, offset + 2 MiB) are one
    /// block, so that a mapping may map them with one 2 MiB leaf: all held by
    /// the same node, this one or an ancestor, physically contiguous from a
    /// 2 MiB aligned frame. Writable when that node is this one and not hidden,
    /// as require_read_page() decides for one page. None when any page is
    /// missing, a zero marker, held elsewhere or out of line, and when the
    /// range does not fit in the node or |offset| is not 2 MiB aligned.
    ///
    /// Asked of the pages because nothing else knows: a block's frames are
    /// independent pages from the moment it is allocated (fill_block_locked()),
    /// any of them may since have been copied or taken, and what a clone sees
    /// mixes its own pages with an ancestor's. The first and the last page are
    /// asked first, which settles most windows that are not one block.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto find_block_locked(u64 offset) -> ustl::Option<FoundBlock>;

    /// The lock of every VmObjectPaged over these pages: over the page tree,
    /// and -- through VmObject::lock(), which is this same mutex for a paged
    /// VMO -- over each such object's list of mappings.
    ///
    /// It lives here, with the pages, because more than one object can sit on
    /// one set of pages: a slice child shares its parent's VmCowPages. Were
    /// the lock in the object, parent and child would each lock the same page
    /// tree with a different mutex, which is no lock at all.
    ///
    /// The mutex is not this object's own but its tree's (HierarchyState), so
    /// that when nodes come to hang under one another one lock covers the
    /// walk from a leaf to the pages it reads. A VmCowPages that has no
    /// relatives is a tree of one, and behaves as before.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto lock() -> sync::Mutex & {
        return m_lock;
    }

    /// Makes |page| this node's page |index|: given the VMM role and inserted
    /// Owned, so that this node frees it when it dies. |page| must be an
    /// allocated frame nobody else owns -- one from the allocator, or one
    /// taken off the reserved books with unwire_frame(). OutOfMem when the
    /// page list cannot grow and AlreadyExists for a slot already full; the
    /// caller still owns |page| then.
    TA_REQUIRES(m_lock)
    auto add_new_page_locked(usize index, VmPage *page) -> Status;

    /// Records that this node's page |index| is zero, with no page behind it:
    /// a zero marker (VmPageList::Slot). A read of it is the shared zero page,
    /// a write gets a zeroed page in its place, it hides whatever the node's
    /// ancestors hold there, and a node with a page source does not ask the
    /// source for it. OutOfMem when the page list cannot grow, AlreadyExists
    /// for a slot that holds a page or a marker already.
    ///
    /// Not for a hidden node, which nothing writes: a marker reaches one only
    /// by moving there with a clone.
    TA_REQUIRES(m_lock)
    auto add_zero_marker_locked(usize index) -> Status;

    /// Stops this node's pages coming from its page source: every request
    /// outstanding is completed, its waiter retries and fails, and a fault on
    /// a page that is not here fails at once instead of asking. The pages
    /// supplied before stay, and keep being read and mapped (decision D5 of
    /// .agents/tasks/user-pager). Nothing for a node with no source.
    TA_REQUIRES(m_lock)
    auto detach_source_locked() -> void;

    /// The owner id of the provider behind this node's page source, none for
    /// a node with no source (PageProvider::owner_id()). The source is set
    /// when the node is made, so this takes no lock.
    [[nodiscard]]
    auto page_source_owner_id() const -> ustl::Option<u64>;

    /// Puts the slots of |pages|, in order, one for each page of
    /// [offset, offset + len), into a node with a page source. Into a slot
    /// that is empty: a page goes in as it is, and a zero marker or a gap goes
    /// in as a zero marker. The gap too, because here an empty slot means "ask
    /// the source", and the source has just said what is there: zero. Into a
    /// slot that already holds a page or a marker nothing goes, since the
    /// content there is already what the source supplied, and a page that
    /// would have is freed. The source hears of each run of slots filled,
    /// which completes the requests waiting on them.
    ///
    /// NotSupported for a node with no source, BadState once the source is
    /// detached, and OutOfMem when the page list cannot grow: what was put in
    /// before that stays, and the rest of |pages| is freed with the list.
    TA_REQUIRES(m_lock)
    auto supply_pages_locked(u64 offset, u64 len, VmPageSpliceList *pages) -> Status;

    /// Takes the content of [offset, offset + len) out of this node into
    /// |pages|, a fresh list as long as the range, at the same places, and
    /// leaves the range reading zero: each page and zero marker moves, and an
    /// offset with neither is a gap. First the range is unmapped from every
    /// mapping of every object over the node (change_range_locked()), since a
    /// frame that leaves has to leave no entry behind.
    ///
    /// In a copy-on-write tree, a page this node only sees through an
    /// ancestor is shared content, and is made the node's own first, as a
    /// write would make it (cow_page()): copied, or taken from the hidden node
    /// when this node is its last viewer. And where a page further up would
    /// show through once the node's slot is empty, the slot keeps a zero
    /// marker, so that what was taken reads as zero and not as the ancestor's
    /// old bytes. A zero marker further up needs nothing: it reads as zero.
    ///
    /// The content does not change until every step that can fail is done:
    /// making the shared pages the node's own (OutOfMem, and the node owns
    /// copies of what it saw, which reads the same), and making room in the
    /// list for every slot that moves (OutOfMem, and nothing moved). From
    /// there nothing allocates: a slot that keeps a marker has its page
    /// swapped for one in place.
    ///
    /// NotSupported for a node with a page source: its pages are the source's
    /// content, not the caller's to move.
    TA_REQUIRES(m_lock)
    auto take_pages_locked(u64 offset, u64 len, VmPageSpliceList *pages) -> Status;

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
    /// One mutex per tree is decision 3 of the vmo-snapshot-child design: the
    /// simplest thing that is correct, where the alternative is one lock per
    /// node and a lock order. Every process that clones one program contends on
    /// the one mutex, and the benchmark that says whether that matters comes
    /// after the feature works.
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

    /// A zeroed frame of |order| for this node, with its flags. A node that can
    /// wait (Gaf::CanWait) may be told to while free memory is short: then
    /// |request| is raised and this answers ShouldWait, and the call retried
    /// after the wait takes the frame the request got. Without a |request|,
    /// from a caller that holds locks it cannot let go of, the same answer is
    /// OutOfMem, as for a waiter that was refused.
    auto alloc_pages(usize order, VmPage **page, AnonymousPageRequest *request) -> Status;

    /// How a page came into this node, which picks its queue
    /// (file_page_locked()): with content of its own (supplied, copied,
    /// wired), or committed in place of zero content.
    enum class Fill : u8 {
        Content,
        ZeroFork,
    };

    /// Files |page|, just put at |index| of this node, on its node's page
    /// queue for its kind, with the backlink to this node: pinned for a
    /// pinned node, reclaimable for a node with a page source, and otherwise
    /// anonymous, zero-fork or not by |fill|. Called with this node's lock
    /// held, right after the page went into the page list, so that a page is
    /// never in the list and on no queue while anyone else can look.
    auto file_page_locked(VmPage *page, PgOff index, Fill fill) -> void;

    /// The pages in this node's list now belong to it: after it took another
    /// node's list whole (a clone's pages, to the new hidden node).
    auto own_all_pages_locked() -> void;

    /// |page| now belongs to |owner|, at |index|, on the queue it is on.
    static auto set_page_owner(VmPage *page, VmCowPages *owner, PgOff index) -> void;

    /// Takes |page| off its queue and drops its backlink: it leaves the node
    /// for a list of pages taken. Before free_page() or a hand-over, since a
    /// frame has one list hook, and the free lists use it too.
    static auto unfile_page(VmPage *page) -> void;

    /// |page|, which a write is about to change in place at |index|, is no
    /// longer content the page source can supply again: if this node has one,
    /// the page leaves the reclaimable queues for the anonymous one, and stays
    /// there. Writing it back is later work, and until then a reclaimer that
    /// took it would throw the write away. A pinned node's page is already
    /// where it belongs.
    TA_REQUIRES(m_lock)
    auto file_dirty_locked(VmPage *page, PgOff index) -> void;

    /// Takes |page| off its queue and gives it back to the allocator.
    static auto free_page(VmPage *page) -> void;

    /// Fills the hole at |offset| with the whole 2 MiB block it lies in, and
    /// answers the block's page at |offset|; null, with nothing changed, when
    /// the node does not take blocks (Flags::LargeBlocks, no parent, no page
    /// source), the block's range does not fit in the node or holds a page or
    /// a zero marker already, or the allocator has no block or the page list
    /// cannot grow. The caller then fills the hole with one page.
    ///
    /// The block is an order-9 allocation made into 512 frames of order 0 as
    /// it arrives, each a page of this node on its own: nothing records that
    /// they were one block. A record would have to be cleared by every path
    /// that takes one of them away, and a head left saying order 9 would make
    /// any free by the frames' own order (free_frames() of a list) free the
    /// whole block from under the other pages. Whether a range is still one
    /// block is asked of the pages when it matters, by their addresses.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto fill_block_locked(u64 offset) -> VmPage *;

    /// What a node sees at an offset, with where it was found: a page, a zero
    /// marker, or nothing. Nothing is what the walk answers when it reached the
    /// top, or a node's limit, without meeting either; it reads as zero, except
    /// in a node with a page source, where it is content not supplied yet.
    struct FoundContent {
        /// The page, or null for a zero marker and for nothing.
        VmPage *page;
        /// Whether what was found is a zero marker: zero, and recorded so.
        bool zero_marker;
        /// The node that holds the page or the marker, this one or an
        /// ancestor, and its offset in that node's coordinates: what a write
        /// needs to take the page out or put a page in the marker's place.
        /// Null and 0 for nothing.
        VmCowPages *owner;
        u64 offset;
    };

    /// What this node sees at |offset|, an offset in its own coordinates: the
    /// content of its own slot if that holds any, and if not the nearest
    /// ancestor's. One walk for both, because they are one question -- what
    /// does this node see here -- and its answer is where the content was
    /// found: a page in the node's own store may be written, and a page found
    /// further up is shared content that a writer has to copy or take first.
    /// A zero marker stops the walk as a page does, which is how it hides the
    /// pages above it.
    ///
    /// The lock is held by every caller. It is not annotated, for the reason
    /// m_pages is not guarded: the callers include Cursor, which the analysis
    /// cannot tie to the lock.
    ///
    /// It loops while the offset is below the node's parent limit, looking in
    /// the node and then adding the parent offset to go up.
    [[nodiscard]]
    auto find_content(u64 offset) -> FoundContent;

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
    /// Unmapping the writer's old read-only mapping on a fork is not done here:
    /// this does not know the mappings.
    [[nodiscard]]
    auto cow_page(u64 offset, FoundContent const &found, AnonymousPageRequest *request)
        -> ktl::Result<VmPage *>;

    /// Whether a page further up the tree shows at |offset| of this node when
    /// its own slot is empty: a page an ancestor holds, within this node's
    /// limit. A zero marker further up is no page. What take_pages_locked()
    /// asks of each slot it empties, to know whether the slot has to keep a
    /// zero marker to hide it.
    TA_REQUIRES(m_lock)
    auto shows_ancestor_page_locked(u64 offset) -> bool;

    /// A leaf that dies is one viewer fewer of every page it sees through its
    /// ancestors: a page it was the only viewer of has none left and is freed,
    /// and the share count of any other drops by one. Pages of its own are the
    /// destructor's, and a page it forked or took is no longer one it sees.
    /// Nothing for a node that is not a viewer, a hidden one.
    TA_REQUIRES(m_lock)
    auto release_viewed_pages_locked() -> void;

    /// This node stops viewing |found|, a page of a hidden ancestor it saw
    /// through an empty slot: the page's share count drops by one, or, when
    /// this node was its last viewer, the page leaves the hidden node and is
    /// freed. What a leaf's death does to each page it sees, and what a zero
    /// marker put over such a page does to that one.
    TA_REQUIRES(m_lock)
    auto stop_viewing_locked(FoundContent const &found) -> void;

    /// A hidden node left with one child is a pointless step for everything
    /// below it to climb, so it is merged into that child. Its pages the child
    /// can see move into the child, the rest are freed because nobody sees them,
    /// and the child takes over its parent link, with its offset and limit
    /// composed with the child's own. The hidden node is then off every list,
    /// holds nothing, has no parent, and is freed by whoever holds the last
    /// reference to it, which takes no lock.
    ///
    /// Moving a page can fail, for the tree that holds it may have to grow, and
    /// then the merge stops where it is: the pages moved are the child's, the
    /// rest are still visible through the hidden node, and the hidden node stays
    /// a step between them until its child dies. Nothing is lost or seen twice.
    ///
    /// The caller holds a reference to this node, and still does when this
    /// returns: the child's own is dropped here and must not be the last.
    /// Nothing here takes a reference to the child.
    TA_REQUIRES(m_lock)
    auto merge_into_only_child_locked() -> void;

    /// Makes a node and adopts it. The one place a VmCowPages is allocated, so
    /// create(), create_child() and the clone do not each spell out the cache
    /// call and its OutOfMem. The node is not linked onto any list: that is
    /// the caller's, under the lock.
    static auto make_node(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy,
                          ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                          Flags flags, ustl::Arc<VmCowPages> *out) -> Status;

    /// When no page sources exists, it will be used in frame allocation request.
    Gaf m_gaf;
    /// Page list that operates within the virtual memory range of the VMO, from [0, N).
    VmPageList m_pages;
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
    /// only in [0, m_parent_limit). Past that it reads as zero.
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
    /// Two things about walking it are enforced, not asked for, both from the
    /// fact that a node's destructor takes the tree's lock to leave this list.
    ///
    ///  - A node on the list may already have lost its last reference: it stays
    ///    here until its destructor gets the lock. Its memory is intact, so its
    ///    fields can be read and changed, but a reference cannot be taken to it
    ///    with Arc::adopt(): RefCounted::retain() stops the program on an object
    ///    that is being destroyed, in every build, where it would otherwise be
    ///    destroyed twice. A walker that has to hold a node asks
    ///    Arc::try_adopt(), which answers empty for one that is dying, and is
    ///    ready for that. No walker needs one today: the merge of a dying node
    ///    into its sibling changes the sibling's fields and nothing else.
    ///  - The last reference to a node must not be released while the lock is
    ///    held. ~VmCowPages() stops the program, with a message, in every build,
    ///    where the thread would otherwise wait for itself. The way out is to
    ///    hand the reference to the caller to drop after the lock.
    ///
    /// Under the tree's lock, like everything else here that more than one
    /// node can see.
    ChildList m_children TA_GUARDED(m_lock);

    /// The objects over these pages. Under the tree's lock. Empty for a hidden
    /// node, which is no object's.
    Attachment::List m_attachments TA_GUARDED(m_lock);
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
/// It carries m_offset and m_end, and steps the first in require_owned_page()
/// and require_read_page().
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
    struct RequireResult {
        VmPage *page;
        bool writable;
    };

    Cursor(VmCowPages *owner, VirtAddr offset, usize size);
    ~Cursor() = default;

    /// Hand back the page for the offset the cursor sits on, then step past it.
    ///
    /// |will_write| picks the discipline: a writer needs a page this object
    /// owns, a reader would be content with shared content. The declaration
    /// used to take no such flag, which is one reason it could never be
    /// defined.
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

    /// The page a read would find at the cursor, if one is there, then step
    /// past it: the node's own, writable, or one it sees through an ancestor,
    /// read-only. Nothing when there is no page -- a hole, a zero marker, a page
    /// a source has not supplied -- and nothing is allocated or asked for to
    /// make one. What a mapping that maps only what is there wants.
    auto maybe_page() -> ustl::Option<RequireResult>;

    /// Hand pages out without marking them used.
    ///
    /// Every page the lookups above hand out is marked used now, which brings
    /// a page a page source supplied back to its node's newest generation: the
    /// record of an access through the physmap, which no page table entry the
    /// aging thread looks at sees. A caller whose access goes through a page
    /// table entry turns that off, because the entry records the access
    /// itself: a hardware fault, and a mapping filled ahead of use. On by
    /// default, because a forgotten mark gets a page in use reclaimed, while
    /// one made twice costs nothing.
    auto disable_mark_accessed() -> void {
        m_mark_accessed = false;
    }

    auto create_read_request(usize nr_pages, PageRequest *request) -> Status;
    auto create_dirty_request(usize nr_pages, PageRequest *request) -> Status;

  private:
    auto convert_reference_to_page(AnonymousPageRequest *request) -> Status;

    /// Marks |result|'s page used, unless disable_mark_accessed() said not to,
    /// and hands |result| back. Every exit that hands a page out goes through
    /// here, so a lookup added later marks too.
    auto handed_out(RequireResult result) -> RequireResult;

    /// Whether a page found in the node's own store may be mapped writable.
    auto writable_here(FoundContent const &found) const -> bool;

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

  public:
    /// Steps past |size| bytes without handing out their pages: what a caller
    /// that mapped them some other way -- a whole block as one leaf -- does to
    /// go on from the next page.
    FORCE_INLINE
    auto skip(usize const size) -> void {
        DEBUG_ASSERT(size <= m_end - m_offset, "the cursor skipped past the range it was made for");
        m_offset += size;
    }

  private:
    VmCowPages *m_target;
    /// Byte offsets into the owning VMO, not addresses in any address space:
    /// |m_offset| is the slot being served and |m_end| is one past the last
    /// byte this cursor was made for. Byte rather than page offsets to match
    /// the rest of the VmObject interface, which is byte-addressed because a
    /// physical VMO need not begin on a page boundary.
    VirtAddr m_offset;
    VirtAddr m_end;
    bool m_mark_accessed;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_COW_PAGES_HPP