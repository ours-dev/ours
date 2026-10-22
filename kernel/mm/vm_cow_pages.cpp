#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/physmap.hpp>
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

VmCowPages::VmCowPages(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy,
                       ustl::Arc<VmCowPages> parent, u64 parent_offset, u64 parent_limit,
                       Flags flags)
    // Initializer order follows the member declaration order in the header:
    // the reference to the mutex can only be taken from a hierarchy that is
    // already held.
    : m_gaf(gaf)
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
/// VmPageMap frees only its tree nodes, decommit() is Unimplemented, and the
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

    m_pages.drain_owned([](VmPage *const page) { free_frames(page, 0); });
}

auto VmCowPages::remove_write_locked(u64 const offset, u64 const len) -> void {
    for (auto &attachment: m_attachments) {
        // The object's lock is this node's, one mutex under two names.
        attachment.m_owner->lock().assert_held();
        attachment.m_owner->remove_write_of_cow_range_locked(offset, len);
    }
}

auto VmCowPages::release_viewed_pages_locked() -> void {
    if (is_hidden()) {
        return;
    }

    // Everything this node sees through its parent: [0, m_parent_limit) in its
    // own coordinates, walked the way a read walks it. A page of its own at an
    // offset shadows what is above, and this node is no viewer of that.
    for (u64 at = 0; at < m_parent_limit; at += PAGE_SIZE) {
        if (m_pages.get_page(PgOff(at >> PAGE_SHIFT))) {
            continue;
        }

        auto const found = find_page(at);
        // A page a node that is not hidden holds belongs to that node, and has
        // no count.
        if (!found.page || !found.owner->is_hidden()) {
            continue;
        }

        auto *const page = found.page;
        if (page->share_count() > 0) {
            page->drop_share();
            continue;
        }

        // The last viewer. Nobody else can see the page, and it is in a hidden
        // node that nobody writes, so it is freed here rather than left until the
        // hidden node goes.
        [[maybe_unused]]
        auto *const taken = found.owner->m_pages.remove_page(PgOff(found.offset >> PAGE_SHIFT));
        DEBUG_ASSERT(taken == page, "the page left a slot other than the one it was found in");
        free_frames(page, 0);
    }
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
    while (auto *const page = m_pages.next_page(next, &index)) {
        next = index + 1;
        u64 const here = static_cast<u64>(index) << PAGE_SHIFT;

        // Not in the survivor's window, or in it but under a page of the survivor's
        // own: nobody sees this page, and it goes.
        bool visible = here >= survivor_at && here - survivor_at < survivor_limit;
        PgOff const there = static_cast<PgOff>((here - survivor_at) >> PAGE_SHIFT);
        if (visible && survivor.m_pages.get_page(there)) {
            visible = false;
        }
        if (!visible) {
            [[maybe_unused]]
            auto *const taken = m_pages.remove_page(index);
            DEBUG_ASSERT(taken == page);
            free_frames(page, 0);
            continue;
        }

        // Only the survivor sees this page now, so it has no count to keep,
        // unless the survivor is itself a hidden node that leaves below it see.
        DEBUG_ASSERT(survivor.is_hidden() || 0 == page->share_count(),
                     "a page with another viewer in a hidden node with one child");

        // Into the survivor first and out of this node after, so that a failed
        // insertion leaves the page where it was. It is the one thing here that
        // can fail, and stopping leaves a consistent tree: see the declaration.
        if (Status::Ok != survivor.m_pages.insert_page(there, page, VmPageMap::Tag::Owned)) {
            return;
        }
        [[maybe_unused]]
        auto *const taken = m_pages.remove_page(index);
        DEBUG_ASSERT(taken == page);
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

auto VmCowPages::make_node(Gaf const gaf, usize const size, ustl::Arc<HierarchyState> hierarchy,
                           ustl::Arc<VmCowPages> parent, u64 const parent_offset,
                           u64 const parent_limit, Flags const flags,
                           ustl::Arc<VmCowPages> *const out) -> Status {
    auto cow_pages = VmCowPagesCache::get(GAF_KERNEL, gaf, size, ustl::move(hierarchy),
                                          ustl::move(parent), parent_offset, parent_limit, flags);
    if (!cow_pages) {
        return Status::OutOfMem;
    }
    *out = ustl::Arc<Self>::adopt(cow_pages);

    return Status::Ok;
}

auto VmCowPages::create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status {
    // A tree of one: this node is its own root, so it starts its own lock.
    auto hierarchy = ustl::Arc<HierarchyState>::adopt(new (GAF_KERNEL) HierarchyState());
    if (!hierarchy) {
        return Status::OutOfMem;
    }

    return make_node(gaf, size, ustl::move(hierarchy), ustl::Arc<VmCowPages>{}, 0, 0, Flags::None,
                     out);
}

auto VmCowPages::with_source(ustl::Arc<PageSource> src, Gaf const gaf, usize const size,
                             ustl::Arc<VmCowPages> *const out) -> Status {
    DEBUG_ASSERT(src);

    ustl::Arc<VmCowPages> node;
    if (auto const status = create(gaf, size, &node); Status::Ok != status) {
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

    if (!ustl::mm::is_aligned(parent_offset, PAGE_SIZE) ||
        !ustl::mm::is_aligned(parent_limit, PAGE_SIZE) || !ustl::mm::is_aligned(size, PAGE_SIZE)) {
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
    auto const gaf = parent->m_gaf;
    auto hierarchy = parent->m_hierarchy;

    // Held before it is linked, so that whatever happens between the two steps
    // the node has an owner, and the destructor that would take it off the list
    // is the one that runs.
    ustl::Arc<VmCowPages> node;
    if (auto const status = make_node(gaf, size, ustl::move(hierarchy), ustl::move(parent),
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

    if (is_hidden()) {
        return Status::BadState;
    }
    // Not yet: see create_child().
    if (m_page_source) {
        return Status::NotSupported;
    }
    if (!ustl::mm::is_aligned(offset, PAGE_SIZE) || !ustl::mm::is_aligned(size, PAGE_SIZE)) {
        return Status::InvArgs;
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
    // own in the window and its parent is hidden: then everything this node
    // shows in the window is in the hidden parent, and the clone can see it
    // there directly. This is what keeps a program started many times from
    // growing a chain: after the first clone this node holds nothing, and every
    // later clone finds the same hidden node.
    //
    // |at| is the window's start in the coordinates of |chosen|; each step up
    // compares it with that node's limit before it adds that node's offset, as
    // find_page() does.
    VmCowPages *chosen = this;
    u64 at = offset;
    while (chosen->m_parent && chosen->m_parent->is_hidden() &&
           !chosen->m_pages.has_page_in(PgOff(at >> PAGE_SHIFT),
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
                make_node(m_gaf, size, m_hierarchy, ustl::Arc<VmCowPages>::adopt(chosen), at,
                          visible, Flags::None, &clone);
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
        if (auto const status = make_node(m_gaf, m_size, m_hierarchy, ustl::Arc<VmCowPages>{}, 0, 0,
                                          Flags::Hidden, &hidden_node);
            Status::Ok != status) {
            return status;
        }
        if (auto const status =
                make_node(m_gaf, size, m_hierarchy, hidden_node, at, visible, Flags::None, &clone);
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
        remove_write_locked(0, m_size);

        hidden_node->m_parent = m_parent;
        hidden_node->m_parent_offset = m_parent_offset;
        hidden_node->m_parent_limit = m_parent_limit;
        hidden_node->m_pages.take_all_from(m_pages);
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
    // takes, so that what is counted is exactly what a read of the clone finds.
    for (u64 at_clone = 0; at_clone < visible; at_clone += PAGE_SIZE) {
        if (auto *const page = clone->find_page(at_clone).page) {
            page->add_share();
        }
    }

    *out = ustl::move(clone);

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

auto VmCowPages::add_new_page_locked(usize const index, VmPage *const page) -> Status {
    DEBUG_ASSERT(index < m_size / PAGE_SIZE, "page {} is past the node's end", index);
    page->become<page::VmmBody>();
    return m_pages.insert_page(index, page, VmPageMap::Tag::Owned);
}

/// The source is told run by run, not once for the whole range: a page whose
/// slot was already full was not supplied now, and counting it would complete
/// a request for it that is waiting on a different supply. That can happen
/// only to a page that was supplied twice, which leaves nobody waiting on it.
auto VmCowPages::supply_pages_locked(u64 const offset, u64 const len, VmPage::List *const pages)
    -> Status {
    DEBUG_ASSERT(ustl::mm::is_aligned(offset, PAGE_SIZE) && ustl::mm::is_aligned(len, PAGE_SIZE));
    DEBUG_ASSERT(offset <= m_size && len <= m_size - offset, "the range is past the node's end");

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
        auto front = pages->front();
        if (!front) {
            end_run();
            return Status::InvArgs;
        }
        auto *const page = &*front;
        pages->pop_front();

        auto const status = add_new_page_locked(at >> PAGE_SHIFT, page);
        if (Status::Ok == status) {
            if (0 == run_len) {
                run_start = at;
            }
            run_len += PAGE_SIZE;
            continue;
        }

        end_run();
        if (Status::AlreadyExists == status) {
            free_frames(page, 0);
            continue;
        }
        pages->push_front(*page);
        return status;
    }

    end_run();
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

auto VmCowPages::find_page(u64 offset) -> FoundPage {
    auto *node = this;

    // |offset| is in the coordinates of |node|; the limit and the parent offset
    // are that node's own, so the offset is compared with the limit first and
    // only then moved into the parent's coordinates.
    while (true) {
        if (auto *const page = node->m_pages.get_page(PgOff(offset >> PAGE_SHIFT))) {
            return {.page = page, .owner = node, .offset = offset};
        }
        if (!node->m_parent || offset >= node->m_parent_limit) {
            return {.page = nullptr, .owner = nullptr, .offset = 0};
        }
        offset += node->m_parent_offset;
        node = node->m_parent.as_ptr_mut();
    }
}

auto VmCowPages::cow_page(u64 const offset, FoundPage const &found) -> ktl::Result<VmPage *> {
    auto *const source = found.page;
    auto *const holder = found.owner;
    DEBUG_ASSERT(source && holder);

    // The last viewer takes the page. Put into this node's store before it is
    // taken out of the hidden node's, so that when the insertion fails, which
    // is the only way this can, the page is still where it was.
    if (holder->is_hidden() && 0 == source->share_count()) {
        if (auto const status =
                m_pages.insert_page(PgOff(offset >> PAGE_SHIFT), source, VmPageMap::Tag::Owned);
            Status::Ok != status) {
            return ktl::err(status);
        }
        [[maybe_unused]]
        auto *const taken = holder->m_pages.remove_page(PgOff(found.offset >> PAGE_SHIFT));
        DEBUG_ASSERT(taken == source, "the page left a slot other than the one it was found in");
        return ktl::ok(source);
    }

    // Someone else still sees the page, or it is not the hidden node's to give:
    // copy it. alloc_pages() zeroes the frame and the copy overwrites all of it;
    // that first fill is wasted work, noted in docs/repairs/260930/mm.md.
    VmPage *copy = nullptr;
    if (auto const status = alloc_pages(0, &copy, nullptr); Status::Ok != status) {
        return ktl::err(status);
    }
    std::memcpy(PhysMap::phys_to_virt<u8>(frame_to_phys(copy)),
                PhysMap::phys_to_virt<u8>(frame_to_phys(source)), PAGE_SIZE);

    if (auto const status =
            m_pages.insert_page(PgOff(offset >> PAGE_SHIFT), copy, VmPageMap::Tag::Owned);
        Status::Ok != status) {
        free_frames(copy, 0);
        return ktl::err(status);
    }

    // Only a hidden node's page carries a count; a page a plain node holds
    // has no other viewer to lose.
    if (holder->is_hidden()) {
        source->drop_share();
    }
    return ktl::ok(copy);
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
    // writer never touches the one the others see. Not found at all -- a hole,
    // or the zero tail past a limit -- there is nothing to copy, and it falls
    // through to be allocated as a hole is.
    auto const found = m_target->find_page(m_offset);
    if (found.page) {
        if (found.owner == m_target) {
            advance();
            return ktl::ok(RequireResult{.page = found.page, .writable = true});
        }

        auto owned = m_target->cow_page(m_offset, found);
        if (!owned) {
            return ktl::err(owned.unwrap_err());
        }
        advance();
        return ktl::ok(RequireResult{.page = *owned, .writable = true});
    }

    // Nothing here. Who answers that depends on whether anyone can:
    //
    //   - With a page source, the content lives somewhere this kernel cannot
    //     reach synchronously, so the only honest answer is to raise a request
    //     and tell the caller to wait: ShouldWait, and the caller drops its
    //     locks, waits on the request and comes back. A write needs the content
    //     too, so it raises the same read request.
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

    // What the node sees here, wherever it is held. Writable only when it is the
    // node's own page, and not even then for a hidden node, whose pages are
    // shared with every node below it and are never written through it. A page
    // found further up is on loan and read-only, and allocates nothing.
    auto const found = m_target->find_page(m_offset);
    if (found.page) {
        advance();
        return ktl::ok(RequireResult{
            .page = found.page, .writable = found.owner == m_target && !m_target->is_hidden()});
    }

    // Nothing there. A node that hangs under another, and a hidden one, reads
    // that as the shared zero page, read-only, and allocates nothing: what the
    // walk found none of includes an offset past the node's limit, where the
    // parent's content does not reach.
    if (m_target->m_parent || m_target->is_hidden()) {
        advance();
        return ktl::ok(RequireResult{.page = get_zero_page(), .writable = false});
    }

    // A read of an offset nobody has populated reads zero. It could be
    // answered with a zero marker in the page map instead, so that a
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


/// What the cursor is currently sitting on.
///
/// VmPageMap stores pages and nothing else: it has no marker for a zero slot
/// and no reference entry for a page owned by a parent VMO, because a parent's
/// page is found by walking up (find_page()), not recorded below. So a
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

/// The run asked for starts at the cursor and is no longer than |nr_pages|,
/// the cursor's range or the node, and stops before the first page the node
/// already holds: a request for a page that is there would be answered by a
/// supply the node throws away.
auto VmCowPages::Cursor::create_read_request(usize const nr_pages, PageRequest *const page_request)
    -> Status {
    DEBUG_ASSERT(m_target->m_page_source);
    DEBUG_ASSERT(nr_pages > 0);
    DEBUG_ASSERT(!m_target->m_pages.get_page(index()), "a read request for a page that is there");

    auto const limit =
        std::min<u64>({nr_pages * PAGE_SIZE, m_end - m_offset, m_target->m_size - m_offset});
    u64 len = PAGE_SIZE;
    while (len < limit && !m_target->m_pages.get_page(PgOff((m_offset + len) >> PAGE_SHIFT))) {
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