#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>

#include <cstring>

#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
using VmCowPagesCache = ObjectCache::Instance<VmCowPages>;

namespace {
    /// The queues of the node |page|'s frame is on: a page is filed with the
    /// memory it is, wherever its VMO is used from.
    auto queues_of(VmPage const *page) -> PageQueues & {
        auto *const node = PmNode::node(page->nid());
        DEBUG_ASSERT(node, "a VMO page on node {}, which does not exist", page->nid());
        return node->page_queues();
    }

    /// Whether every byte of |page| is zero, read through the physmap. Not
    /// atomic as a whole: a program writing the page meanwhile may be seen
    /// half way, which dedup_zero_page() allows for.
    auto is_zero_filled(VmPage const *page) -> bool {
        auto const *const words =
            reinterpret_cast<u64 const *>(PhysMap::phys_to_virt(frame_to_phys(page)));
        for (usize i = 0; i < PAGE_SIZE / sizeof(u64); ++i) {
            if (words[i] != 0) {
                return false;
            }
        }
        return true;
    }
} // namespace

auto VmCowPages::file_page_locked(VmPage *const page, PgOff const index, Fill const fill) -> void {
    auto &queues = queues_of(page);
    if (!!(m_flags & Flags::Pinned)) {
        queues.set_pinned(page, this, index);
    } else if (m_page_source && Fill::Written != fill) {
        queues.set_reclaimable(page, this, index);
    } else if (Fill::ZeroFork == fill) {
        queues.set_anonymous_zero_fork(page, this, index);
    } else {
        queues.set_anonymous(page, this, index);
    }
}

auto VmCowPages::file_dirty_locked(VmPage *const page, PgOff const index) -> void {
    if (!m_page_source || !!(m_flags & Flags::Pinned)) {
        return;
    }
    auto &queues = queues_of(page);
    if (PageQueues::PageKind::Reclaimable == queues.kind_of(*page)) {
        queues.set_anonymous(page, this, index);
    }
}

auto VmCowPages::reclaim_page(PgOff const index, VmPage *const page) -> ReclaimResult {
    Guard<sync::Mutex> guard{m_lock};

    auto const slot = m_pages.lookup(index);
    if (!slot.is_page() || slot.page() != page) {
        return ReclaimResult::Gone;
    }
    auto &queues = queues_of(page);
    if (PageQueues::PageKind::Reclaimable != queues.kind_of(*page)) {
        // Written, or pinned, since the reclaimer chose it: not on the ring any more.
        return ReclaimResult::Kept;
    }
    if (is_hidden() || !m_page_source) {
        // Nothing files such a page as reclaimable. Were it so, the page would
        // be the oldest again at once, and the reclaimer would look at it for ever.
        queues.mark_accessed(page);
        return ReclaimResult::Kept;
    }

    // The entries go first, as in take_pages_locked(): a frame that leaves
    // must leave no mapping behind. No fault can map the page again until the
    // lock is let go, since every fault takes it to find the page, and by
    // then the slot is a hole.
    range_change_locked(usize(index) << PAGE_SHIFT, PAGE_SIZE, RangeOp::Unmap);

    [[maybe_unused]]
    auto *const removed = m_pages.remove_page(index);
    DEBUG_ASSERT(removed == page, "the page at {} left the list under the lock", index);
    free_page(page);
    return ReclaimResult::Evicted;
}

auto VmCowPages::dedup_zero_page(PgOff const index, VmPage *const page) -> ReclaimResult {
    Guard<sync::Mutex> guard{m_lock};

    auto const slot = m_pages.lookup(index);
    if (!slot.is_page() || slot.page() != page) {
        return ReclaimResult::Gone;
    }
    auto &queues = queues_of(page);
    if (PageQueues::PageKind::AnonymousZeroFork != queues.kind_of(*page)) {
        return ReclaimResult::Kept;
    }

    // Not the reclaimer's again, whatever the reason.
    auto const leave = [&]() {
        queues.set_anonymous(page, this, index);
        return ReclaimResult::Kept;
    };
    // A node with relatives shares what it holds: a hidden node's page is seen
    // by every node below, through mappings that are not this node's to change;
    // a node with children is the same in a tree only a test makes.
    if (is_hidden() || nr_children_locked() != 0 || VmPageList::Tag::Owned != slot.tag()) {
        return leave();
    }
    if (!is_zero_filled(page)) {
        return leave();
    }

    // The page looks zero. Take Write from every mapping, and look again: it
    // cannot change from here, so what is seen now is what will be freed.
    auto const offset = usize(index) << PAGE_SHIFT;
    range_change_locked(offset, PAGE_SIZE, RangeOp::RemoveWrite);
    if (!is_zero_filled(page)) {
        return leave();
    }

    // The entries go before the frame does, as in reclaim_page(), and the slot
    // keeps the content as a marker, which allocates nothing.
    range_change_locked(offset, PAGE_SIZE, RangeOp::Unmap);
    [[maybe_unused]]
    auto *const taken = m_pages.replace_page_with_zero_marker(index);
    DEBUG_ASSERT(taken == page, "the page at {} left the list under the lock", index);
    free_page(page);
    return ReclaimResult::Evicted;
}

auto VmCowPages::own_all_pages_locked() -> void {
    PgOff index = 0;
    PgOff next = 0;
    while (true) {
        auto const slot = m_pages.next_slot(next, &index);
        if (slot.is_empty()) {
            break;
        }
        next = index + 1;
        if (auto *const page = slot.page()) {
            set_page_owner(page, this, index);
        }
    }
}

auto VmCowPages::set_page_owner(VmPage *const page, VmCowPages *const owner, PgOff const index)
    -> void {
    queues_of(page).set_owner(page, owner, index);
}

auto VmCowPages::unfile_page(VmPage *const page) -> void {
    queues_of(page).remove(page);
}

auto VmCowPages::free_page(VmPage *const page) -> void {
    DEBUG_ASSERT(page->pinned_count() == 0, "a pinned page is being freed");
    unfile_page(page);
    free_frames(page, 0);
}

VmCowPages::VmCowPages(Gaf alloc_flags, usize size, ustl::Arc<HierarchyState> hierarchy,
                       ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                       Flags flags)
    // Initializer order follows the member declaration order in the header:
    // the reference to the mutex can only be taken from a hierarchy that is
    // already held.
    : m_alloc_flags(alloc_flags)
    , m_size(size)
    , m_parent(ustl::move(parent))
    , m_parent_offset(parent_offset)
    , m_parent_limit(parent_limit)
    , m_flags(flags)
    , m_hierarchy(ustl::move(hierarchy))
    , m_lock(m_hierarchy->lock()) {}

/// Returns every page this object owns.
///
/// It used to be `= default`, which meant a VMO's pages were never given back:
/// VmPageList frees only its tree nodes, decommit() is Unimplemented, and the
/// single free_frames() on this path is the one that undoes a failed insert.
/// Nothing noticed while the only VMOs were kernel stacks and the userboot
/// image, because neither is ever destroyed -- but it makes any allocator
/// built on VMOs leak every byte it frees, which is exactly what heap::vfree()
/// would have done. It now drains every page it owns out of the map and frees
/// each one, as its last step.
///
/// Before that, off its parent's list of children, under the tree's lock: the
/// list holds a bare link to this node and would otherwise be left pointing
/// at freed memory, and the lock is what keeps a walk of that list from
/// seeing the node half unlinked. The node can take it here because its
/// hierarchy is still held (m_hierarchy goes with the members, after this
/// body) and because nobody can be holding the lock waiting for this node:
/// the last reference is already gone. It may sleep, as any lock of this tree
/// may, so a VmCowPages must not be released from a context that cannot.
///
/// A node with no parent is on no list. Its own list of children is empty by
/// now, since each child held a reference to it.
VmCowPages::~VmCowPages() {
    DEBUG_ASSERT(m_representative == nullptr, "a COW node still has a VMO view");

    // Closed before the pages go, so the provider stops being asked for pages
    // of a node that is going away, and every thread still waiting on one is
    // woken: its retry finds the source detached, or the mapping gone.
    if (m_page_source) {
        m_page_source->close();
    }

    if (m_parent) {
        // Stops the program, in every build, where the thread would otherwise
        // wait for itself for ever: the last reference to this node was released
        // by a thread that holds the tree's lock, and the lock taken below is
        // that one. The mutex is not recursive, and lockdep only reports it
        // in the builds that have lockdep.
        ASSERT(m_lock.holder() != task::Thread::Current::get(),
               "the last reference to a VmCowPages was released with its tree's lock held");

        Guard guard{m_lock};
        m_parent->lock().assert_held();

        // In this order, in one hold of the lock. What this node sees has to be
        // let go of while its parent chain is still what it saw; it leaves the
        // list before anyone can count it a child; and whether the parent is now
        // left with one is only known after it has left.
        release_viewed_pages_locked();

        // Not linked when the node was made and dropped before anything linked
        // it: a clone that runs out of memory half way frees the node it had
        // already made.
        if (m_sibling_hook.is_linked()) {
            m_parent->m_children.erase(*this);
        }

        if (m_parent->is_hidden() && 1 == m_parent->m_children.size()) {
            m_parent->merge_into_only_child_locked();
        }
    }

    DEBUG_ASSERT(m_nr_pinned_pages == 0, "a node with {} pinned pages is being destroyed",
                 m_nr_pinned_pages);

    // Each page off its queue, under the queue's lock, before this node's
    // storage goes: a reclaimer that found a page on a queue reads its owner
    // under that lock, and the owner must still be there to be read
    // (Rc::try_adopt()).
    m_pages.drain_owned([](VmPage *const page) { free_page(page); });
}

auto VmCowPages::range_change_locked(usize const offset, usize const len, RangeOp const op)
    -> void {
    if (auto *const representative = m_representative) {
        representative->lock().assert_held();
        representative->operate_views_locked(offset, len, op);
    }
}

auto VmCowPages::release_viewed_pages_locked() -> void {
    if (is_hidden()) {
        return;
    }

    // Everything this node sees through its parent: [0, m_parent_limit) in its
    // own coordinates, walked the way a read walks it. A page or a zero marker
    // of its own at an offset shadows what is above, and this node is no
    // viewer of that.
    for (u64 at = 0; at < m_parent_limit; at += PAGE_SIZE) {
        if (!m_pages.lookup(PgOff(at >> PAGE_SHIFT)).is_empty()) {
            continue;
        }

        auto const found = find_content(at);
        // A page a node that is not hidden holds belongs to that node, and has
        // no count; nor has a zero marker, which holds no page.
        if (!found.page || !found.owner->is_hidden()) {
            continue;
        }
        stop_viewing_locked(found);
    }
}

auto VmCowPages::stop_viewing_locked(FoundContent const &found) -> void {
    DEBUG_ASSERT(found.page && found.owner->is_hidden(), "only a hidden node's page has viewers");
    auto *const page = found.page;
    if (page->share_count() > 0) {
        page->drop_share();
        return;
    }

    // The last viewer. Nobody else can see the page, and it is in a hidden
    // node that nobody writes, so it is freed here rather than left until the
    // hidden node goes.
    [[maybe_unused]]
    auto *const taken = found.owner->m_pages.remove_page(PgOff(found.offset >> PAGE_SHIFT));
    DEBUG_ASSERT(taken == page, "the page left a slot other than the one it was found in");
    free_page(page);
}

auto VmCowPages::merge_into_only_child_locked() -> void {
    DEBUG_ASSERT(is_hidden() && 1 == m_children.size());
    // A reference to the survivor is never taken: it may be a node whose last
    // reference has gone and whose destructor is waiting for the lock held here,
    // and its fields are all this needs.
    VmCowPages &survivor = m_children.front().value();

    // Where the survivor sees this node: [survivor_at, survivor_at + survivor_limit).
    u64 const survivor_at = survivor.m_parent_offset;
    u64 const survivor_limit = survivor.m_parent_limit;

    PgOff index = 0;
    PgOff next = 0;
    while (true) {
        auto const slot = m_pages.next_slot(next, &index);
        if (slot.is_empty()) {
            break;
        }
        next = index + 1;
        u64 const here = static_cast<u64>(index) << PAGE_SHIFT;

        // Not in the survivor's window, or in it but under a page or a zero
        // marker of the survivor's own: nobody sees this slot, and it goes.
        bool visible = here >= survivor_at && here - survivor_at < survivor_limit;
        PgOff const there = static_cast<PgOff>((here - survivor_at) >> PAGE_SHIFT);
        if (visible && !survivor.m_pages.lookup(there).is_empty()) {
            visible = false;
        }

        // A zero marker the survivor sees moves into it: once this node is gone
        // the survivor hangs under this node's parent, and the marker is what
        // keeps the parent's page there hidden from it. Into the survivor first,
        // as a page goes, so that running out of memory stops the merge with
        // the marker still here, still hiding the page.
        if (slot.is_zero_marker()) {
            if (visible && Status::Ok != survivor.m_pages.insert_zero_marker(there)) {
                return;
            }
            [[maybe_unused]]
            auto const removed = m_pages.remove_zero_marker(index);
            DEBUG_ASSERT(removed);
            continue;
        }

        auto *const page = slot.page();
        if (!visible) {
            [[maybe_unused]]
            auto *const taken = m_pages.remove_page(index);
            DEBUG_ASSERT(taken == page);
            free_page(page);
            continue;
        }

        // Only the survivor sees this page now, so it has no count to keep,
        // unless the survivor is itself a hidden node that leaves below it see.
        DEBUG_ASSERT(survivor.is_hidden() || 0 == page->share_count(),
                     "a page with another viewer in a hidden node with one child");

        // Into the survivor first and out of this node after, so that a failed
        // insertion leaves the page where it was. It is the one thing here that
        // can fail, and stopping leaves a consistent tree: see the declaration.
        if (Status::Ok != survivor.m_pages.insert_page(there, page, VmPageList::Tag::Owned)) {
            return;
        }
        [[maybe_unused]]
        auto *const taken = m_pages.remove_page(index);
        DEBUG_ASSERT(taken == page);
        set_page_owner(page, &survivor, there);
    }

    // What the survivor sees of this node's parent: its window in this node,
    // cut at the end of this node's own window in the parent. The limit is
    // computed from the survivor's offset as it was, before it is composed.
    u64 window = 0;
    if (survivor_at < m_parent_limit) {
        window = std::min<u64>(survivor_limit, m_parent_limit - survivor_at);
    }

    // The survivor takes this node's place on the parent's list, and off this one.
    m_children.erase(survivor);
    if (m_parent) {
        m_parent->lock().assert_held();
        m_parent->m_children.erase(*this);
        m_parent->m_children.push_back(survivor);
    }

    survivor.m_parent_offset = survivor_at + m_parent_offset;
    survivor.m_parent_limit = window;
    // Moves this node's parent, so that it has none and its destructor takes no
    // lock, and drops the survivor's reference to this node. That is not the last
    // one: the caller holds another.
    survivor.m_parent = ustl::move(m_parent);
    m_parent_offset = 0;
    m_parent_limit = 0;
}

auto VmCowPages::make_node(Gaf const alloc_flags, usize const size,
                           ustl::Arc<HierarchyState> hierarchy, ustl::Arc<VmCowPages> parent,
                           u64 const parent_offset, u64 const parent_limit, Flags const flags,
                           ustl::Arc<VmCowPages> *const out) -> Status {
    auto cow_pages = VmCowPagesCache::get(ALLOC_KERNEL, alloc_flags, size, ustl::move(hierarchy),
                                          ustl::move(parent), parent_offset, parent_limit, flags);
    if (!cow_pages) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(cow_pages);

    return Status::Ok;
}

auto VmCowPages::create(Gaf alloc_flags, usize size, ustl::Arc<VmCowPages> *out) -> Status {
    return create(alloc_flags, size, Flags::None, out);
}

auto VmCowPages::create(Gaf const alloc_flags, usize const size, Flags const flags,
                        ustl::Arc<VmCowPages> *const out) -> Status {
    DEBUG_ASSERT(!(flags & Flags::Hidden), "a node with no relatives is never hidden");

    // A tree of one: this node is its own root, so it starts its own lock.
    auto hierarchy = ustl::Arc<HierarchyState>::adopt(new (ALLOC_KERNEL) HierarchyState());
    if (!hierarchy) {
        return Status::OutOfMem;
    }

    return make_node(alloc_flags, size, ustl::move(hierarchy), ustl::Arc<VmCowPages>{}, 0, 0, flags,
                     out);
}

auto VmCowPages::with_source(ustl::Arc<PageSource> src, Gaf const alloc_flags, usize const size,
                             ustl::Arc<VmCowPages> *const out) -> Status {
    DEBUG_ASSERT(src);

    ustl::Arc<VmCowPages> node;
    if (auto const status = create(alloc_flags, size, &node); Status::Ok != status) {
        return status;
    }
    // Before anyone else can see the node, so no lock: nothing can look a page
    // up in it yet.
    node->m_page_source = ustl::move(src);
    *out = ustl::move(node);

    return Status::Ok;
}

auto VmCowPages::create_child(ustl::Arc<VmCowPages> parent, u64 const parent_offset,
                              u64 const parent_limit, usize const size, Flags const flags,
                              ustl::Arc<VmCowPages> *out) -> Status {
    DEBUG_ASSERT(parent);

    // A lookup from a child walks up and stops at the root; it never asks the
    // root's source, so a child would read every page not yet supplied as zero.
    // The source is set when the node is made and never changes, so it is read
    // without the lock.
    if (parent->m_page_source) {
        return Status::NotSupported;
    }

    if (!ustl::is_aligned(parent_offset, PAGE_SIZE) || !ustl::is_aligned(parent_limit, PAGE_SIZE) ||
        !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    // Clamped, so that what a child claims to see of its parent is never more
    // than the child can hold nor more than the parent has from |parent_offset|
    // on. Read without the lock: a paged object's size is fixed when it is made
    // (nothing can resize one), and this reads nothing else of the parent.
    u64 limit = std::min<u64>(parent_limit, size);
    if (parent_offset >= parent->m_size) {
        limit = 0;
    } else {
        limit = std::min<u64>(limit, parent->m_size - parent_offset);
    }

    // Copied out before |parent| is handed on: the node that takes it does not
    // exist yet, and what it borrows -- the flags and the lock -- is the
    // parent's.
    auto const alloc_flags = parent->m_alloc_flags;
    auto hierarchy = parent->m_hierarchy;

    // Held before it is linked, so that whatever happens between the two steps
    // the node has an owner, and the destructor that would take it off the list
    // is the one that runs.
    ustl::Arc<VmCowPages> node;
    if (auto const status = make_node(alloc_flags, size, ustl::move(hierarchy), ustl::move(parent),
                                      parent_offset, limit, flags, &node);
        Status::Ok != status) {
        return status;
    }

    // Onto the parent's list under the tree's lock, which is also the child's.
    // Nothing can fail from here, and the list needs no allocation: the hook is
    // in the node.
    {
        Guard guard{node->m_lock};
        node->m_parent->lock().assert_held();
        node->m_parent->m_children.push_back(*node);
    }
    *out = ustl::move(node);

    return Status::Ok;
}

/// Where the clone hangs is decided first, and everything that can fail --
/// the two allocations -- is done before anything is changed, so a failure
/// leaves the tree as it was and the tail of the function, which links and
/// counts, cannot fail at all.
///
/// The analysis is off for the body: it names a lock by the object it is a
/// member of, and this touches the lists of several nodes, every one of them
/// in this tree and so under the one mutex that is required on entry (asserted
/// below in the debug build).
TA_NO_THREAD_SAFETY_ANALYSIS
auto VmCowPages::create_clone_locked(u64 const offset, usize const size, u64 const end,
                                     ustl::Arc<VmCowPages> *const out) -> Status {
    m_lock.assert_held();

    if (is_hidden() || !!(m_flags & Flags::Pinned)) {
        return Status::BadState;
    }
    // Not yet: see create_child().
    if (m_page_source) {
        return Status::NotSupported;
    }
    if (!ustl::is_aligned(offset, PAGE_SIZE) || !ustl::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    // A clone makes this node's pages shared, by moving them under a hidden
    // node, and a pinned page must stay in the frame a device writes: the whole
    // node, and not only the window, since the hidden node takes all it holds.
    if (m_nr_pinned_pages != 0) {
        return Status::BadState;
    }

    // How much of the window is this node's own content: past this node's end
    // there is nothing to see, and the clone reads zero there.
    u64 const window_end = std::min<u64>(end, m_size);
    u64 visible = 0;
    if (offset < window_end) {
        visible = std::min<u64>(size, window_end - offset);
    }

    // Look for a node the clone can hang under without a new hidden node above
    // this one. Going up is worth it only while the node holds nothing of its
    // own in the window -- no page, and no zero marker, which hides the page
    // above it -- and its parent is hidden: then everything this node
    // shows in the window is in the hidden parent, and the clone can see it
    // there directly. This is what keeps a program started many times from
    // growing a chain: after the first clone this node holds nothing, and every
    // later clone finds the same hidden node.
    //
    // |at| is the window's start in the coordinates of |chosen|; each step up
    // compares it with that node's limit before it adds that node's offset, as
    // find_content() does.
    VmCowPages *chosen = this;
    u64 at = offset;
    while (chosen->m_parent && chosen->m_parent->is_hidden() &&
           !chosen->m_pages.has_content_in(PgOff(at >> PAGE_SHIFT),
                                           PgOff((at + visible) >> PAGE_SHIFT))) {
        if (at >= chosen->m_parent_limit) {
            visible = 0;
        } else {
            visible = std::min<u64>(visible, chosen->m_parent_limit - at);
        }
        at += chosen->m_parent_offset;
        chosen = chosen->m_parent.as_ptr_mut();
    }

    ustl::Arc<VmCowPages> clone;
    if (chosen->is_hidden()) {
        // Hangs under a hidden node that is already there. The window is
        // translated into that node's coordinates by the walk above.
        if (auto const status =
                make_node(m_alloc_flags, size, m_hierarchy, ustl::Arc<VmCowPages>::adopt(chosen),
                          at, visible, Flags::None, &clone);
            Status::Ok != status) {
            return status;
        }
        chosen->m_children.push_back(*clone);
    } else {
        // A new hidden node between this one and its parent. It stands in this
        // node's place, so it is this node's size and has this node's parent
        // and window of it; this node then hangs under it, with everything it
        // held, and so does the clone.
        //
        // It is made with no parent, and is given this node's only in the part
        // that cannot fail. A node with a parent takes the tree's lock in its
        // destructor (to leave the parent's list), and the caller holds that
        // lock: if the clone's allocation failed after this one's, freeing a
        // node that already had a parent would take the lock a second time on
        // the same thread and never return.
        ustl::Arc<VmCowPages> hidden_node;
        if (auto const status =
                make_node(m_alloc_flags, m_size, m_hierarchy, ustl::Arc<VmCowPages>{}, 0, 0,
                          Flags::Hidden, &hidden_node);
            Status::Ok != status) {
            return status;
        }
        if (auto const status = make_node(m_alloc_flags, size, m_hierarchy, hidden_node, at,
                                          visible, Flags::None, &clone);
            Status::Ok != status) {
            // |hidden_node| goes with this return: never linked, and with no
            // parent, so its destructor takes no lock.
            return status;
        }

        // Nothing fails from here.
        //
        // First, Write comes off every mapping of the pages that are about to be
        // shared, of every object over this node: a writable entry left over a
        // page that another node is about to see would write to all of them.
        // Under the lock, so no fault can map a page writable in between.
        range_change_locked(0, m_size, RangeOp::RemoveWrite);

        hidden_node->m_parent = m_parent;
        hidden_node->m_parent_offset = m_parent_offset;
        hidden_node->m_parent_limit = m_parent_limit;
        hidden_node->m_pages.take_all_from(m_pages);
        hidden_node->own_all_pages_locked();
        if (m_parent) {
            // Not an in-place replacement: the list is in no particular order.
            m_parent->m_children.erase(*this);
            m_parent->m_children.push_back(*hidden_node);
        }
        m_parent = hidden_node;
        m_parent_offset = 0;
        m_parent_limit = m_size;
        hidden_node->m_children.push_back(*this);
        hidden_node->m_children.push_back(*clone);
    }

    // The clone is one more node that sees every page it can reach through its
    // ancestors, all of them held by hidden nodes now, so each of those pages
    // has one more viewer. Counted after the tree is settled, by the walk a read
    // takes, so that what is counted is exactly what a read of the clone finds:
    // a page behind a zero marker is not one the clone sees, and a marker holds
    // no page to count.
    for (u64 at_clone = 0; at_clone < visible; at_clone += PAGE_SIZE) {
        if (auto *const page = clone->find_content(at_clone).page) {
            page->add_share();
        }
    }

    *out = ustl::move(clone);

    return Status::Ok;
}

auto VmCowPages::make_cursor(usize const offset, usize const size) -> Cursor {
    return Cursor(this, offset, size);
}

auto VmCowPages::find_block_locked(u64 const offset) -> ustl::Option<FoundBlock> {
    constexpr usize BLOCK = BLOCK_SIZE;
    constexpr usize NR_PAGES = BLOCK / PAGE_SIZE;

    if (!ustl::is_aligned(offset, BLOCK) || offset + BLOCK > m_size) {
        return ustl::NONE;
    }
    auto const first = find_content(offset);
    if (first.page == nullptr) {
        return ustl::NONE;
    }
    auto const base = frame_to_phys(first.page);
    if (!ustl::is_aligned(base, BLOCK)) {
        return ustl::NONE;
    }
    // The page at |index| is the block's own: held where the first one is, and
    // the frame right after the one before it.
    auto const in_line = [&](usize const index) {
        auto const found = find_content(offset + index * PAGE_SIZE);
        return found.page != nullptr && found.owner == first.owner &&
               frame_to_phys(found.page) == base + index * PAGE_SIZE;
    };
    if (!in_line(NR_PAGES - 1)) {
        return ustl::NONE;
    }
    for (usize i = 1; i < NR_PAGES - 1; ++i) {
        if (!in_line(i)) {
            return ustl::NONE;
        }
    }
    return ustl::some(FoundBlock{
        .base = base,
        .writable = first.owner == this && !is_hidden(),
    });
}

auto VmCowPages::alloc_pages(usize order, VmPage **page, AnonymousPageRequest *request) -> Status {
    // Gaf::Zero unconditionally, on top of whatever the object was created
    // with. Every byte of a VMO that nobody has supplied reads as zero by
    // definition, and the buddy allocator hands back whatever the previous
    // owner left in the frame -- so without this the first read of a
    // freshly-faulted page returns another owner's data. None of the VMOs
    // built today ask for Gaf::Zero themselves (they all pass ALLOC_KERNEL,
    // which does not imply it), so the guarantee has to live here rather than
    // rest on callers remembering. A page source, once one exists, overwrites
    // the whole page anyway and should be allowed to skip the fill.
    auto const alloc_flags = m_alloc_flags | Gaf::Zero;

    // The frame a wait got, allocated with these same flags, so zeroed too.
    PmFrame *frame = nullptr;
    if (request && request->has_frame()) {
        DEBUG_ASSERT(order == 0, "a wait gets one frame, and a block never waits");
        frame = request->take_frame();
    } else {
        auto result = alloc_frames(alloc_flags, order);
        if (!result) {
            auto const status = result.unwrap_err();
            if (Status::ShouldWait != status) {
                return status;
            }
            // Nobody to wait: refused, as a waiter would be.
            if (!request) {
                return Status::OutOfMem;
            }
            request->make_active(alloc_flags);
            return Status::ShouldWait;
        }
        frame = *result;
    }

    // Fresh from the buddy allocator: declare the role, do not query it.
    frame->become<page::VmmBody>();
    *page = frame;
    return Status::Ok;
}

auto VmCowPages::fill_block_locked(u64 const offset) -> VmPage * {
    constexpr usize BLOCK = BLOCK_SIZE;
    constexpr usize ORDER = 9;
    constexpr usize NR_PAGES = BLOCK / PAGE_SIZE;
    static_assert((usize{1} << ORDER) == NR_PAGES);

    if (!(m_flags & Flags::LargeBlocks) || m_parent || m_page_source) {
        return nullptr;
    }
    auto const start = ustl::round_down(offset, BLOCK);
    if (start + BLOCK > m_size) {
        return nullptr;
    }
    auto const first = PgOff(start >> PAGE_SHIFT);
    if (m_pages.has_content_in(first, first + NR_PAGES)) {
        return nullptr;
    }

    // Through the gate like any page of this node, but never waiting for it:
    // turned away, the hole gets one page instead, and that page waits.
    auto allocated = alloc_frames(m_alloc_flags | Gaf::Zero, ORDER);
    if (!allocated) {
        return nullptr;
    }
    auto *const block = *allocated;
    // 512 frames of order 0 from here on, as if each had been allocated alone.
    // The descriptors of one block are contiguous: a block never crosses the
    // memmap's sections (FrameBuddy).
    for (usize i = 0; i < NR_PAGES; ++i) {
        block[i].set_order(0);
        block[i].become<page::VmmBody>();
    }

    // Into the page list one by one. A failure gives back every page this put
    // in and every frame of the block, each alone, as they now are.
    for (usize i = 0; i < NR_PAGES; ++i) {
        if (Status::Ok != m_pages.insert_page(first + i, &block[i], VmPageList::Tag::Owned)) {
            for (usize j = 0; j < i; ++j) {
                [[maybe_unused]]
                auto *const taken = m_pages.remove_page(first + j);
                DEBUG_ASSERT(taken == &block[j]);
            }
            for (usize j = 0; j < NR_PAGES; ++j) {
                free_frames(&block[j], 0);
            }
            return nullptr;
        }
    }
    // Filed once all are in: a failure above gives back pages no queue has.
    for (usize i = 0; i < NR_PAGES; ++i) {
        file_page_locked(&block[i], first + i, Fill::ZeroFork);
    }
    return &block[(offset - start) >> PAGE_SHIFT];
}

auto VmCowPages::add_new_page_locked(usize const index, VmPage *const page) -> Status {
    DEBUG_ASSERT(index < m_size / PAGE_SIZE, "page {} is past the node's end", index);
    page->become<page::VmmBody>();
    if (auto const status = m_pages.insert_page(index, page, VmPageList::Tag::Owned);
        Status::Ok != status) {
        return status;
    }
    file_page_locked(page, index, Fill::Content);
    return Status::Ok;
}

/// A marker over an empty slot hides what the node saw there through its
/// ancestors. When that is a page of a hidden node, the node is one of the
/// page's viewers, and stops being one: the share count every other path that
/// hides a page keeps (cow_page(), release_viewed_pages_locked()). It is
/// looked up before the marker goes in, since afterwards the node sees the
/// marker, and given up only once the marker is in, since a marker that could
/// not be put changes nothing.
auto VmCowPages::add_zero_marker_locked(usize const index) -> Status {
    DEBUG_ASSERT(index < m_size / PAGE_SIZE, "page {} is past the node's end", index);
    DEBUG_ASSERT(!is_hidden(), "a hidden node is never written");

    FoundContent hidden{};
    if (m_parent && m_pages.lookup(PgOff(index)).is_empty()) {
        auto const found = find_content(u64{index} * PAGE_SIZE);
        if (found.page && found.owner->is_hidden()) {
            hidden = found;
        }
    }

    auto const status = m_pages.insert_zero_marker(index);
    if (Status::Ok == status && hidden.page) {
        stop_viewing_locked(hidden);
    }
    return status;
}

/// The source is told run by run, not once for the whole range: a page whose
/// slot was already full was not supplied now, and counting it would complete
/// a request for it that is waiting on a different supply. That can happen
/// only to a page that was supplied twice, which leaves nobody waiting on it.
auto VmCowPages::supply_pages_locked(u64 const offset, u64 const len, VmPageSpliceList *const pages)
    -> Status {
    DEBUG_ASSERT(ustl::is_aligned(offset, PAGE_SIZE) && ustl::is_aligned(len, PAGE_SIZE));
    DEBUG_ASSERT(offset <= m_size && len <= m_size - offset, "the range is past the node's end");
    DEBUG_ASSERT(pages->is_unread() && pages->nr_pages() == len / PAGE_SIZE,
                 "the list is not the range's");

    if (!m_page_source) {
        return Status::NotSupported;
    }
    if (m_page_source->is_detached()) {
        return Status::BadState;
    }

    u64 run_start = offset;
    u64 run_len = 0;
    auto const end_run = [&] {
        if (run_len != 0) {
            m_page_source->on_pages_supplied(run_start, run_len);
        }
        run_len = 0;
    };

    for (u64 at = offset; at < offset + len; at += PAGE_SIZE) {
        auto const slot = pages->pop();
        auto const index = at >> PAGE_SHIFT;
        auto status = Status::Ok;
        if (auto *const page = slot.page()) {
            status = add_new_page_locked(index, page);
            // Popped, so the page is this call's to give back when it does not
            // go in: the slot was full, or the map could not grow.
            if (Status::Ok != status) {
                free_frames(page, 0);
            }
        } else {
            // A zero marker or a gap: zero either way, and said by the source.
            status = add_zero_marker_locked(index);
        }

        if (Status::Ok == status) {
            if (0 == run_len) {
                run_start = at;
            }
            run_len += PAGE_SIZE;
            continue;
        }

        end_run();
        if (Status::AlreadyExists == status) {
            continue;
        }
        return status;
    }

    end_run();
    return Status::Ok;
}

auto VmCowPages::detach_source_locked() -> void {
    // Under this node's lock, which a supply holds from its check that the
    // source is attached to its last page: a supply in flight finishes before
    // the detach, or finds it.
    if (m_page_source) {
        m_page_source->detach();
    }
}

auto VmCowPages::page_source_owner_id() const -> ustl::Option<u64> {
    if (!m_page_source) {
        return ustl::NONE;
    }
    return m_page_source->owner_id();
}

auto VmCowPages::take_pages_locked(usize const offset, usize const len,
                                   VmPageSpliceList *const pages) -> Status {
    DEBUG_ASSERT(ustl::is_aligned(offset, PAGE_SIZE) && ustl::is_aligned(len, PAGE_SIZE));
    DEBUG_ASSERT(offset <= m_size && len <= m_size - offset, "the range is past the node's end");
    DEBUG_ASSERT(pages->is_fresh() && pages->nr_pages() == len / PAGE_SIZE,
                 "the list is not a fresh one of the range's length");
    DEBUG_ASSERT(!is_hidden(), "a hidden node is no object's to take from");

    if (!!(m_flags & Flags::Pinned)) {
        return Status::BadState;
    }
    if (m_page_source) {
        return Status::NotSupported;
    }
    // A pinned page cannot be taken out: a device holds its address. Looked at
    // only when there is one, so a node with no pin pays nothing.
    if (m_nr_pinned_pages != 0) {
        PgOff at = 0;
        for (auto from = PgOff(offset >> PAGE_SHIFT); from < PgOff((offset + len) >> PAGE_SHIFT);
             from = at + 1) {
            auto const slot = m_pages.next_slot(from, &at);
            if (slot.is_empty() || at >= PgOff((offset + len) >> PAGE_SHIFT)) {
                break;
            }
            if (slot.is_page() && slot.page()->pinned_count() != 0) {
                return Status::BadState;
            }
        }
    }

    // The entries go first: a frame that leaves this node must not stay
    // reachable through a mapping of it. Harmless if a step below fails: the
    // pages fault back in. No fault can map one again until this returns,
    // since every fault takes this lock to find a page.
    range_change_locked(offset, len, RangeOp::Unmap);

    // A page this node sees through an ancestor is made its own, the way a
    // write makes it. Only then is it the node's to hand over: the ancestor's
    // page is still what the other nodes below it see.
    if (m_parent) {
        for (u64 at = offset; at < offset + len; at += PAGE_SIZE) {
            auto const found = find_content(at);
            if (!found.page || found.owner == this) {
                continue;
            }
            // No request: this holds the node's lock throughout and cannot wait.
            if (auto owned = cow_page(at, found, nullptr); !owned) {
                return owned.unwrap_err();
            }
        }
    }

    auto const first = PgOff(offset >> PAGE_SHIFT);
    auto const last = PgOff((offset + len) >> PAGE_SHIFT);

    // Room for every slot that moves, before any does: a slot cannot be put
    // back once it is out of the list, so the one allocation is made first.
    usize nr_held = 0;
    PgOff index = 0;
    for (auto from = first; from < last; from = index + 1) {
        auto const slot = m_pages.next_slot(from, &index);
        if (slot.is_empty() || index >= last) {
            break;
        }
        nr_held += 1;
    }
    if (auto const status = pages->reserve(nr_held); Status::Ok != status) {
        return status;
    }

    // Nothing fails from here.
    for (auto from = first; from < last; from = index + 1) {
        auto const slot = m_pages.next_slot(from, &index);
        if (slot.is_empty() || index >= last) {
            break;
        }
        auto const at = static_cast<usize>(index - first);
        // Whether the slot, once empty, would show a page further up: then
        // it keeps a zero marker to go on hiding it.
        auto const hides = shows_ancestor_page_locked(u64(index) << PAGE_SHIFT);
        if (slot.is_zero_marker()) {
            [[maybe_unused]]
            auto const appended = pages->append_zero_marker(at);
            DEBUG_ASSERT(Status::Ok == appended, "the room made above ran out");
            if (!hides) {
                [[maybe_unused]]
                auto const removed = m_pages.remove_zero_marker(index);
                DEBUG_ASSERT(removed);
            }
            continue;
        }
        DEBUG_ASSERT(slot.tag() == VmPageList::Tag::Owned, "a page on loan is not this node's");
        VmPage *page = nullptr;
        if (hides) {
            page = m_pages.replace_page_with_zero_marker(index);
        } else {
            page = m_pages.remove_page(index);
        }
        unfile_page(page);
        [[maybe_unused]]
        auto const appended = pages->append_page(at, page);
        DEBUG_ASSERT(Status::Ok == appended, "the room made above ran out");
    }
    return Status::Ok;
}

auto VmCowPages::shows_ancestor_page_locked(u64 const offset) -> bool {
    if (!m_parent || offset >= m_parent_limit) {
        return false;
    }
    return m_parent->find_content(offset + m_parent_offset).page != nullptr;
}

auto VmCowPages::commit_range_locked(usize const offset, usize const size, CommitMode const mode,
                                     MultiPageRequest &request, usize &nr_done) -> Status {
    nr_done = 0;
    if (size == 0 || size > m_size || offset > m_size - size) {
        return Status::InvArgs;
    }

    auto cursor = make_cursor(offset, size);
    auto const walk = [&](auto const &finish_page) -> Status {
        for (usize at = 0; at < size; at += PAGE_SIZE) {
            auto const result = cursor.require_owned_page(1, &request);
            if (!result) {
                return result.unwrap_err();
            }
            auto const status = finish_page(*result->page, PgOff((offset + at) >> PAGE_SHIFT));
            if (Status::Ok != status) {
                return status;
            }
            nr_done += 1;
        }
        return Status::Ok;
    };

    if (mode == CommitMode::Pin) {
        // The cursor makes the page the node's own before a device may write it.
        return walk([this](VmPage &page, PgOff const index) -> Status {
            if (!page.increase_pinned()) {
                return Status::NoResource;
            }
            if (page.pinned_count() == 1) {
                // The reclaimer rechecks the queue under this lock.
                m_lock.assert_held();
                queues_of(&page).set_pinned(&page, this, index);
                m_nr_pinned_pages += 1;
            }
            return Status::Ok;
        });
    }
    return walk([](VmPage &, PgOff) -> Status { return Status::Ok; });
}

auto VmCowPages::unpin_range_locked(usize const offset, usize const size) -> Status {
    if (!size) {
        return Status::InvArgs;
    }
    if (offset + size > m_size) {
        return Status::InvArgs;
    }

    // All or nothing: every page is looked at before any pin is undone.
    for (usize at = 0; at < size; at += PAGE_SIZE) {
        auto const slot = m_pages.lookup(PgOff((offset + at) >> PAGE_SHIFT));
        if (!slot.is_page() || slot.page()->pinned_count() == 0) {
            return Status::BadState;
        }
    }

    for (usize at = 0; at < size; at += PAGE_SIZE) {
        auto const index = PgOff((offset + at) >> PAGE_SHIFT);
        auto *const page = m_pages.lookup(index).page();
        page->decrease_pinned();
        if (page->pinned_count() == 0) {
            m_nr_pinned_pages -= 1;
            // Filed as its kind says, as when it was put here: a pinned node's
            // pages go back on the pinned queue. A pager's page does not become
            // reclaimable: the pin handed it to a device that may have written
            // it, which is why committing it for pinning went through
            // require_owned_page() and so marked it written. Only the pinned
            // queue held that mark meanwhile, and an unpin that filed the page
            // as the source's content would let the reclaimer evict what the
            // device wrote and read the old content back.
            file_page_locked(page, index, Fill::Written);
        }
    }
    return Status::Ok;
}

auto VmCowPages::find_content(u64 offset) -> FoundContent {
    auto *node = this;

    // |offset| is in the coordinates of |node|; the limit and the parent offset
    // are that node's own, so the offset is compared with the limit first and
    // only then moved into the parent's coordinates.
    while (true) {
        if (auto const slot = node->m_pages.lookup(PgOff(offset >> PAGE_SHIFT)); !slot.is_empty()) {
            return {.page = slot.page(),
                    .zero_marker = slot.is_zero_marker(),
                    .owner = node,
                    .offset = offset};
        }
        if (!node->m_parent || offset >= node->m_parent_limit) {
            return {.page = nullptr, .zero_marker = false, .owner = nullptr, .offset = 0};
        }
        offset += node->m_parent_offset;
        node = node->m_parent.as_ptr_mut();
    }
}

auto VmCowPages::cow_page(u64 const offset, FoundContent const &found,
                          AnonymousPageRequest *const request) -> ktl::Result<VmPage *> {
    m_lock.assert_held();
    auto *const source = found.page;
    auto *const holder = found.owner;
    DEBUG_ASSERT(source && holder);

    // The last viewer takes the page. Put into this node's store before it is
    // taken out of the hidden node's, so that when the insertion fails, which
    // is the only way this can, the page is still where it was.
    if (holder->is_hidden() && 0 == source->share_count()) {
        if (auto const status =
                m_pages.insert_page(PgOff(offset >> PAGE_SHIFT), source, VmPageList::Tag::Owned);
            Status::Ok != status) {
            return ktl::err(status);
        }
        [[maybe_unused]]
        auto *const taken = holder->m_pages.remove_page(PgOff(found.offset >> PAGE_SHIFT));
        DEBUG_ASSERT(taken == source, "the page left a slot other than the one it was found in");
        set_page_owner(source, this, PgOff(offset >> PAGE_SHIFT));
        return ktl::ok(source);
    }

    // Someone else still sees the page, or it is not the hidden node's to give:
    // copy it. alloc_pages() zeroes the frame and the copy overwrites all of it;
    // that first fill is wasted work, noted in docs/repairs/260930/mm.md.
    VmPage *copy = nullptr;
    if (auto const status = alloc_pages(0, &copy, request); Status::Ok != status) {
        return ktl::err(status);
    }
    std::memcpy(PhysMap::phys_to_virt<u8>(frame_to_phys(copy)),
                PhysMap::phys_to_virt<u8>(frame_to_phys(source)), PAGE_SIZE);

    if (auto const status =
            m_pages.insert_page(PgOff(offset >> PAGE_SHIFT), copy, VmPageList::Tag::Owned);
        Status::Ok != status) {
        free_frames(copy, 0);
        return ktl::err(status);
    }
    file_page_locked(copy, PgOff(offset >> PAGE_SHIFT), Fill::Content);
    // Every mapping of this offset may still point at the inherited frame.
    range_change_locked(offset, PAGE_SIZE, RangeOp::Unmap);

    // Only a hidden node's page carries a count; a page a plain node holds
    // has no other viewer to lose.
    if (holder->is_hidden()) {
        source->drop_share();
    }
    return ktl::ok(copy);
}

/// The followings are in class VmCowPages::Cursor.

VmCowPages::Cursor::Cursor(VmCowPages *target, usize offset, usize size)
    // Initializer order follows the member declaration order in the header.
    : m_target(target), m_offset(offset), m_end(offset + size) {}

/// A page found in the node's own store may be written through a mapping, but
/// not in a node with a page source: such a page is mapped read-only, so that
/// the first write faults and comes to require_owned_page(), which is where
/// the page is marked written (file_dirty_locked()). A page mapped writable
/// would be written with nobody told, and a reclaimer would evict it and lose
/// the write. A hidden node's pages are shared, and never written through it.
auto VmCowPages::Cursor::writable_here(FoundContent const &found) const -> bool {
    return found.owner == m_target && !m_target->is_hidden() && !m_target->m_page_source;
}

auto VmCowPages::Cursor::handed_out(RequireResult const result) -> RequireResult {
    // The shared zero page is no node's and on no queue. Any other page goes
    // to the queues, which move it only if it is reclaimable: whether it is
    // can only be read under their lock, since the aging thread rewrites the
    // queue byte under that lock alone.
    if (!!(m_flags & Flags::MarkAccessed) && result.page != get_zero_page()) {
        queues_of(result.page).mark_accessed(result.page);
    }
    return result;
}

auto VmCowPages::Cursor::require_page(bool will_write, usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    return will_write ? require_owned_page(nr_pages, request)
                      : require_read_page(nr_pages, request);
}

auto VmCowPages::Cursor::require_owned_page(usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    m_target->lock().assert_held();
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
    if (m_target->is_hidden()) {
        return ktl::err(Status::BadState);
    }

    // The page this node sees first, before any thought of allocating. This
    // ordering is the whole point of the class: a fault on an offset this VMO
    // already sees must resolve to the page that is already there, or two
    // mappings of the same VMO see different memory and a re-fault silently
    // discards what the first fault wrote.
    //
    // Found in this node's own store, the page is its own and may be written.
    // Found further up, it is shared content: the node gets a page of its own
    // out of it, a copy or the page itself (VmCowPages::cow_page()), and the
    // writer never touches the one the others see. A zero marker, or nothing
    // found at all -- a hole, or the zero tail past a limit -- leaves nothing
    // to copy, and the page is allocated below.
    auto const found = m_target->find_content(m_offset);
    if (found.page) {
        if (found.owner == m_target) {
            m_target->file_dirty_locked(found.page, index());
            advance();
            return ktl::ok(handed_out({.page = found.page, .writable = true}));
        }

        auto owned = m_target->cow_page(m_offset, found, request->get_anonymous());
        if (!owned) {
            return ktl::err(owned.unwrap_err());
        }
        advance();
        return ktl::ok(handed_out({.page = *owned, .writable = true}));
    }

    // A zero marker in this node's own slot: the content is zero and known to
    // be, so nobody is asked, a page source included. A zeroed page takes the
    // marker's place, which allocates nothing in the map, so nothing can fail
    // once the page is there.
    if (found.zero_marker && found.owner == m_target) {
        VmPage *page = nullptr;
        if (auto const status = m_target->alloc_pages(0, &page, request->get_anonymous());
            Status::Ok != status) {
            return ktl::err(status);
        }
        m_target->m_pages.replace_zero_marker(index(), page, VmPageList::Tag::Owned);
        m_target->file_page_locked(page, index(), Fill::ZeroFork);
        m_target->range_change_locked(m_offset, PAGE_SIZE, RangeOp::Unmap);
        advance();
        return ktl::ok(handed_out({.page = page, .writable = true}));
    }

    // Nothing here, or a zero marker further up. Who answers that depends on
    // whether anyone can:
    //
    //   - With a page source and nothing here, the content lives somewhere this
    //     kernel cannot reach synchronously, so the only honest answer is to
    //     raise a request and tell the caller to wait: ShouldWait, and the
    //     caller drops its locks, waits on the request and comes back. A write
    //     needs the content too, so it raises the same read request.
    //   - Anonymous, or under an ancestor's zero marker, there is no one to ask
    //     and nothing to: the content is zero, and zero is something this
    //     kernel can produce on the spot. So the answer is an allocation,
    //     synchronous, never ShouldWait. The ancestor's marker stays, for the
    //     other nodes that see it.
    if (m_target->m_page_source && !found.zero_marker) {
        return ktl::err(create_read_request(nr_pages, request->get_read()));
    }

    // A node that takes 2 MiB blocks fills the hole with the block it lies in,
    // when one fits; otherwise, and for every other node, with one page.
    if (!found.zero_marker) {
        if (auto *const page = m_target->fill_block_locked(m_offset)) {
            advance();
            return ktl::ok(handed_out({.page = page, .writable = true}));
        }
    }

    VmPage *page = nullptr;
    if (auto const status = m_target->alloc_pages(0, &page, request->get_anonymous());
        Status::Ok != status) {
        return ktl::err(status);
    }

    // Recorded before it is handed out, and the page given back if recording
    // fails. An unrecorded page is worse than no page: the caller maps it, the
    // VMO does not know it exists, and the next fault on this same offset
    // allocates a second one over the top of it.
    if (auto const status = m_target->m_pages.insert_page(index(), page, VmPageList::Tag::Owned);
        Status::Ok != status) {
        free_frames(page, 0);
        return ktl::err(status);
    }
    m_target->file_page_locked(page, index(), Fill::ZeroFork);

    // A marker or a hole seen through a parent may already map the zero page.
    if (found.zero_marker || m_target->m_parent) {
        m_target->range_change_locked(m_offset, PAGE_SIZE, RangeOp::Unmap);
    }

    advance();
    return ktl::ok(handed_out({.page = page, .writable = true}));
}

auto VmCowPages::Cursor::require_read_page(usize nr_pages, MultiPageRequest *request)
    -> ktl::Result<RequireResult> {
    DEBUG_ASSERT(m_offset < m_end, "the cursor stepped past the range it was made for");

    // What the node sees here, wherever it is held. Writable only when it is the
    // node's own page, and not even then for a hidden node, whose pages are
    // shared with every node below it and are never written through it. A page
    // found further up is on loan and read-only, and allocates nothing.
    auto const found = m_target->find_content(m_offset);
    if (found.page) {
        advance();
        return ktl::ok(handed_out({.page = found.page, .writable = writable_here(found)}));
    }

    // A zero marker, in this node or further up: the shared zero page,
    // read-only, whoever holds the marker. Nothing is allocated, and a page
    // source is not asked: the marker is the content it already gave.
    if (found.zero_marker) {
        advance();
        return ktl::ok(handed_out({.page = get_zero_page(), .writable = false}));
    }

    // Nothing there. A node that hangs under another, and a hidden one, reads
    // that as the shared zero page, read-only, and allocates nothing: what the
    // walk found none of includes an offset past the node's limit, where the
    // parent's content does not reach.
    if (m_target->m_parent || m_target->is_hidden()) {
        advance();
        return ktl::ok(handed_out({.page = get_zero_page(), .writable = false}));
    }

    // A read of an offset nobody has populated reads zero. It could be
    // answered with a zero marker in the page list instead, so that a
    // read-only walk over a sparse VMO costs no memory at all and only a later
    // write turns the marker into a page.
    //
    // A node with no relatives still commits a real zeroed page and hands that
    // back, though ZERO_PAGE and the writable bit now exist: identical in what
    // the reader observes, more expensive in footprint, and the only answer
    // that is safe until the fault path maps by |writable|. Handing the shared
    // page to a caller that maps it as it maps every page would let a write
    // through, and every reader of every hole would see every writer.
    return require_owned_page(nr_pages, request);
}

auto VmCowPages::Cursor::maybe_page() -> ustl::Option<RequireResult> {
    DEBUG_ASSERT(m_offset < m_end, "the cursor stepped past the range it was made for");

    // As require_read_page() answers a page that is there, and nothing else.
    auto const found = m_target->find_content(m_offset);
    advance();
    if (!found.page) {
        return ustl::NONE;
    }
    return ustl::some(handed_out({.page = found.page, .writable = writable_here(found)}));
}

/// What the cursor is currently sitting on, in the node's own slot.
///
/// A slot holds a page, a zero marker, or nothing (VmPageList::Slot). It has no
/// reference entry for a page owned by a parent VMO, because a parent's page is
/// found by walking up (find_content()), not recorded below, so
/// is_type_reference() answers no.
auto VmCowPages::Cursor::is_type_reference() const -> bool {
    return false;
}

auto VmCowPages::Cursor::convert_reference_to_page(AnonymousPageRequest *request) -> Status {
    // Unreachable while is_type_reference() is always false.
    return Status::Unimplemented;
}

/// The run asked for starts at the cursor and is no longer than |nr_pages|,
/// the cursor's range or the node, and stops before the first slot the node
/// already has content in, a page or a zero marker: a request for it would be
/// answered by a supply the node throws away.
auto VmCowPages::Cursor::create_read_request(usize const nr_pages, PageRequest *const page_request)
    -> Status {
    DEBUG_ASSERT(m_target->m_page_source);
    DEBUG_ASSERT(nr_pages > 0);
    DEBUG_ASSERT(m_target->m_pages.lookup(index()).is_empty(),
                 "a read request for a page that is there");

    auto const limit =
        std::min<u64>({nr_pages * PAGE_SIZE, m_end - m_offset, m_target->m_size - m_offset});
    u64 len = PAGE_SIZE;
    while (len < limit &&
           m_target->m_pages.lookup(PgOff((m_offset + len) >> PAGE_SHIFT)).is_empty()) {
        len += PAGE_SIZE;
    }

    return m_target->m_page_source->get_pages(m_offset, len, page_request);
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
