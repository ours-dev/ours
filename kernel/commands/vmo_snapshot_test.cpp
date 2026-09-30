/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmosnapshot.test`: check what a VmCowPages that hangs under another one
// sees, by building small trees by hand and asking their cursors for pages.
//
// What it covers, and only that:
//
//   - a read of a node with a parent answers with the nearest ancestor's page,
//     on loan and read-only, and allocates nothing in the node;
//   - what an ancestor does not have, and what lies past a node's limit,
//     reads as the shared zero page -- which is checked to be all zeros, so
//     this is also the one test of ZERO_PAGE;
//   - the parent offset and the limit compose down a chain of nodes, each
//     with its own;
//   - a hidden node hands its pages out read-only and refuses a writer;
//   - every node of a tree answers the same lock, and a node that has lost its
//     handle to its parent still has it;
//   - a node with no relatives still answers a read of a hole as it always
//     did, with a page of its own.
//
// What it cannot cover yet: a node's own page winning over its ancestors'
// (a node with a parent has no way to get a page of its own until a write
// copies one), a write, a clone, a destruction that merges. Each of those is a
// later step of .agents/tasks/vmo-snapshot-child and adds its checks here.
//
// Why a console command: the tree has no in-kernel unittest runtime yet (see
// usercopy.test), and these are kernel objects that no host test can build.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vmm.hpp>

#include <cstring>

#include <gktl/command.hpp>
#include <ktl/result.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using mm::VmCowPages;
    using mm::VmPage;
    using Node = ustl::Arc<VmCowPages>;
    using Found = VmCowPages::Cursor::RequireResult;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("vmosnapshot.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmosnapshot.test: FAIL  {}", name);
    }

    // The analysis cannot tie `node->lock()` to the `m_lock` that make_cursor()
    // requires: it sees two expressions through two calls of Arc::operator->().
    // The lock is taken on the line above and held to the end of the function.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto lookup(Node &node, bool const will_write, usize const page_index) -> ktl::Result<Found> {
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(page_index * PAGE_SIZE, PAGE_SIZE);
        if (!cursor) {
            return ktl::err(cursor.unwrap_err());
        }
        mm::MultiPageRequest request;
        return cursor->require_page(will_write, 1, &request);
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pages(Node &node) -> usize {
        Guard guard{node->lock()};
        return node->nr_pages_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_children(Node &node) -> usize {
        Guard guard{node->lock()};
        return node->nr_children_locked();
    }

    // The clone helpers hold the tree's lock through `tree`, any node in it, and
    // look at another node of the same tree through a plain pointer: the
    // analysis cannot tell that the two are under one mutex.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto parent_of(Node &node) -> VmCowPages const * {
        Guard guard{node->lock()};
        return node->parent_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto pages_in(Node &tree, VmCowPages const *const node) -> usize {
        Guard guard{tree->lock()};
        return node->nr_pages_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto children_in(Node &tree, VmCowPages const *const node) -> usize {
        Guard guard{tree->lock()};
        return node->nr_children_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto clone_raw(Node &source, u64 const offset, usize const size, Node &out) -> Status {
        Guard guard{source->lock()};
        return source->create_clone_locked(offset, size, &out);
    }

    auto clone_of(Node &source, usize const offset_pages, usize const pages, Node &out) -> Status {
        return clone_raw(source, offset_pages * PAGE_SIZE, pages * PAGE_SIZE, out);
    }

    auto bytes_of(VmPage *const page) -> u8 * {
        return mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(page));
    }

    /// Writes |marker| over a whole page of |node| through the cursor, the way
    /// a write would, and answers the page.
    auto fill(Node &node, usize const page_index, u8 const marker) -> VmPage * {
        auto found = lookup(node, true, page_index);
        if (!found) {
            return nullptr;
        }
        std::memset(bytes_of(found->page), marker, PAGE_SIZE);
        return found->page;
    }

    auto holds_only(VmPage *const page, u8 const marker) -> bool {
        auto const *const bytes = bytes_of(page);
        for (usize i = 0; i < PAGE_SIZE; ++i) {
            if (bytes[i] != marker) {
                return false;
            }
        }
        return true;
    }

    /// The page |node| answers a read of |page_index| with, read-only and on
    /// loan from |lender|; and nothing was allocated for it in |node|.
    auto reads_lenders_page(Node &node, usize const page_index, VmPage *const lender) -> bool {
        auto const before = nr_pages(node);
        auto found = lookup(node, false, page_index);
        return found && found->page == lender && !found->writable && nr_pages(node) == before;
    }

    auto reads_zero_page(Node &node, usize const page_index) -> bool {
        auto const before = nr_pages(node);
        auto found = lookup(node, false, page_index);
        return found && found->page == mm::get_zero_page() && !found->writable &&
               nr_pages(node) == before;
    }

    auto make(Node &out, usize const pages) -> bool {
        return VmCowPages::create(mm::GAF_USER, pages * PAGE_SIZE, &out) == Status::Ok;
    }

    auto make_child(Node const &parent, usize const parent_page, usize const limit_pages,
                    usize const pages, bool const hidden, Node &out) -> bool {
        auto flags = VmCowPages::Flags::None;
        if (hidden) {
            flags = VmCowPages::Flags::Hidden;
        }
        return VmCowPages::create_child(parent, parent_page * PAGE_SIZE, limit_pages * PAGE_SIZE,
                                        pages * PAGE_SIZE, flags, &out) == Status::Ok;
    }

    auto vmosnapshot_test_command(int, char **) -> int {
        Tally tally;

        // A parent of three pages: 0 and 1 hold something, 2 is a hole.
        Node parent;
        if (!make(parent, 3)) {
            log::error("vmosnapshot.test: could not make the parent");
            return 1;
        }
        auto *const p0 = fill(parent, 0, 0xA0);
        auto *const p1 = fill(parent, 1, 0xA1);
        verdict(tally, p0 && p1 && p0 != p1 && nr_pages(parent) == 2,
                "the parent holds two pages and a hole");
        if (tally.failed != 0) {
            log::error("vmosnapshot.test: FAILED -- setup");
            return 1;
        }

        // The zero page, on its own: every later "reads as zero" is a claim
        // about this one page.
        verdict(tally, mm::get_zero_page() != nullptr && holds_only(mm::get_zero_page(), 0),
                "ZERO_PAGE exists and is all zeros");

        // A child larger than its parent: sees all three of the parent's pages,
        // and has two more that the parent cannot reach.
        Node child;
        verdict(tally, make_child(parent, 0, 3, 5, false, child), "a child of the parent is made");
        verdict(tally, &child->lock() == &parent->lock(),
                "the child takes the parent's lock, not one of its own");
        verdict(tally, reads_lenders_page(child, 0, p0) && holds_only(p0, 0xA0),
                "the child reads the parent's page 0, read-only, and allocates nothing");
        verdict(tally, reads_lenders_page(child, 1, p1), "and page 1");
        verdict(tally, reads_zero_page(child, 2),
                "a page the parent has a hole for reads as the zero page");
        verdict(tally, reads_zero_page(child, 3) && reads_zero_page(child, 4),
                "and so does everything past the parent's end");
        verdict(tally, nr_pages(child) == 0, "and the child still holds no page of its own");

        // The parent offset: the child's page 0 is the parent's page 1, and its
        // limit cuts it off after two pages.
        Node shifted;
        verdict(tally, make_child(parent, 1, 2, 3, false, shifted), "a child at a parent offset");
        verdict(tally, reads_lenders_page(shifted, 0, p1), "its page 0 is the parent's page 1");
        verdict(tally, reads_zero_page(shifted, 1), "its page 1 is the parent's hole, so zero");
        verdict(tally, reads_zero_page(shifted, 2), "and its page 2 is past its limit");

        // A limit the caller did not check: longer than the child and than what
        // the parent has from that offset. It is clamped, not refused.
        Node clamped;
        verdict(tally, make_child(parent, 1, 100, 8, false, clamped),
                "a child with a limit longer than either side is made");
        verdict(tally, reads_lenders_page(clamped, 0, p1) && reads_zero_page(clamped, 7),
                "and reads as if the limit had been clamped");

        // A misaligned argument is refused.
        Node refused;
        verdict(tally,
                VmCowPages::create_child(parent, 1, PAGE_SIZE, PAGE_SIZE, VmCowPages::Flags::None,
                                         &refused) == Status::InvArgs,
                "an offset that is not page aligned is refused");

        // A chain: the grandchild's offset and limit compose with its parent's.
        Node grandchild;
        verdict(tally, make_child(child, 0, 5, 5, false, grandchild),
                "a child of the child is made");
        verdict(tally, &grandchild->lock() == &parent->lock(),
                "and it is in the same tree, with the same lock");
        verdict(tally,
                reads_lenders_page(grandchild, 0, p0) && reads_lenders_page(grandchild, 1, p1),
                "it reads the pages two hops up");
        verdict(tally, reads_zero_page(grandchild, 3),
                "the middle node's limit stops the walk: page 3 is past the child's limit");
        Node offset_chain;
        verdict(tally, make_child(shifted, 0, 2, 2, false, offset_chain),
                "a child of the shifted child is made");
        verdict(tally, reads_lenders_page(offset_chain, 0, p1),
                "its page 0 is the shifted child's page 0, which is the parent's page 1");

        // Two children of one parent see the same page.
        Node sibling;
        verdict(tally, make_child(parent, 0, 3, 3, false, sibling), "a second child is made");
        verdict(tally, reads_lenders_page(sibling, 0, p0) && reads_lenders_page(child, 0, p0),
                "two children of one parent read the same page");

        // A hidden node.
        Node hidden;
        verdict(tally, make_child(parent, 0, 3, 3, true, hidden) && hidden->is_hidden(),
                "a hidden child is made");
        verdict(tally, !child->is_hidden(), "and an ordinary one is not hidden");
        verdict(tally,
                lookup(hidden, true, 0).is_err() &&
                    lookup(hidden, true, 0).unwrap_err() == Status::BadState,
                "a writer is refused by a hidden node, even where it has a page to show");
        verdict(tally, reads_lenders_page(hidden, 0, p0) && reads_zero_page(hidden, 2),
                "a hidden node reads through to its parent like any other");
        Node below_hidden;
        verdict(tally,
                make_child(hidden, 0, 3, 3, false, below_hidden) &&
                    reads_lenders_page(below_hidden, 1, p1),
                "and a node below it reads through both");

        // A node with no relatives is what it always was.
        Node alone;
        verdict(tally, make(alone, 2), "a node with no relatives is made");
        verdict(tally, &alone->lock() != &parent->lock(), "and it has a lock of its own");
        auto const first = lookup(alone, false, 1);
        verdict(tally,
                first && first->writable && first->page != mm::get_zero_page() &&
                    nr_pages(alone) == 1 && holds_only(first->page, 0),
                "a read of its hole commits a zeroed page of its own, writable");
        auto const again = lookup(alone, false, 1);
        verdict(tally,
                again && again->page == first->page && again->writable && nr_pages(alone) == 1,
                "and the next read finds that page");

        // The list of children: each node made under a parent is on its list,
        // and leaves it when it dies.
        Node family;
        Node kid_a;
        Node kid_b;
        Node kid_c;
        verdict(tally,
                make(family, 2) && make_child(family, 0, 2, 2, false, kid_a) &&
                    make_child(family, 0, 2, 2, false, kid_b) &&
                    make_child(family, 1, 1, 1, true, kid_c),
                "a parent and three children are made");
        verdict(tally, nr_children(family) == 3, "the parent lists all three");
        verdict(tally, nr_children(kid_a) == 0, "and a child with none of its own lists none");
        kid_b = nullptr;
        verdict(tally, nr_children(family) == 2, "a child that dies leaves the list");
        Node kid_a_child;
        verdict(tally, make_child(kid_a, 0, 2, 2, false, kid_a_child),
                "a child of a child is made");
        verdict(tally, nr_children(kid_a) == 1 && nr_children(family) == 2,
                "it is on its own parent's list, not on its grandparent's");
        kid_a = nullptr;
        verdict(tally, nr_children(family) == 2 && reads_zero_page(kid_a_child, 0),
                "a child whose parent handle is dropped stays on the grandparent's list, alive");
        kid_a_child = nullptr;
        verdict(tally, nr_children(family) == 1,
                "and leaves it when it dies, taking its parent with it");
        kid_c = nullptr;
        verdict(tally, nr_children(family) == 0, "the parent ends up with none");

        // The clone. A node that holds pages is cloned: its pages move into a
        // new hidden node above it, and it and the clone hang under that.
        Node origin;
        verdict(tally, make(origin, 3), "a node to clone is made");
        auto *const o0 = fill(origin, 0, 0xC0);
        auto *const o1 = fill(origin, 1, 0xC1);
        verdict(tally, o0 && o1 && pages_in(origin, origin.as_ptr()) == 2,
                "it holds two pages and a hole");
        Node clone1;
        verdict(tally, clone_of(origin, 0, 3, clone1) == Status::Ok, "it is cloned");
        auto const *const shared = parent_of(origin);
        verdict(tally, shared != nullptr && shared->is_hidden() && parent_of(clone1) == shared,
                "the node and its clone hang under one hidden node");
        verdict(tally, &clone1->lock() == &origin->lock(), "in one tree, under one lock");
        verdict(tally, pages_in(origin, origin.as_ptr()) == 0 && pages_in(origin, shared) == 2,
                "the pages moved up into the hidden node, and the node holds none");
        verdict(tally, children_in(origin, shared) == 2, "which lists the node and the clone");
        verdict(tally, o0->share_count() == 1 && o1->share_count() == 1,
                "each page both of them see has one more viewer");
        verdict(tally,
                reads_lenders_page(origin, 0, o0) && reads_lenders_page(clone1, 0, o0) &&
                    reads_lenders_page(clone1, 1, o1) && reads_zero_page(clone1, 2),
                "both read the same pages, and the hole as zero");
        verdict(tally, holds_only(o0, 0xC0) && holds_only(o1, 0xC1),
                "and the bytes are what was written before the clone");

        // A second clone of the same node finds it holds nothing and hangs
        // under the same hidden node: no new one, and no chain.
        Node clone2;
        verdict(tally, clone_of(origin, 0, 3, clone2) == Status::Ok, "it is cloned again");
        verdict(tally,
                parent_of(clone2) == shared && parent_of(origin) == shared &&
                    children_in(origin, shared) == 3 && pages_in(origin, shared) == 2,
                "the second clone hangs under the same hidden node");
        verdict(tally, o0->share_count() == 2 && o1->share_count() == 2,
                "and the pages have one more viewer again");

        // A window: only the page it can see gains a viewer.
        Node window;
        verdict(tally, clone_of(origin, 1, 1, window) == Status::Ok, "a one-page window is cloned");
        verdict(tally,
                reads_lenders_page(window, 0, o1) && o0->share_count() == 2 &&
                    o1->share_count() == 3,
                "its page 0 is the node's page 1, and only that page gained a viewer");

        // A window longer than the node: the rest reads as zero.
        Node longer;
        verdict(tally, clone_of(origin, 0, 5, longer) == Status::Ok,
                "a clone longer than the node");
        verdict(tally,
                reads_lenders_page(longer, 1, o1) && reads_zero_page(longer, 2) &&
                    reads_zero_page(longer, 4) && o0->share_count() == 3 && o1->share_count() == 4,
                "reads the node, then zero past its end");

        // A clone of a clone: the clone holds nothing, so this one hangs there too.
        Node of_clone;
        verdict(tally, clone_of(clone1, 0, 3, of_clone) == Status::Ok, "a clone of a clone");
        verdict(tally,
                parent_of(of_clone) == shared && children_in(origin, shared) == 6 &&
                    reads_lenders_page(of_clone, 0, o0),
                "hangs under the same hidden node, six nodes under it now");

        // Refused.
        Node refused_clone;
        verdict(tally, clone_of(origin, 0, 1, refused_clone) == Status::Ok && refused_clone,
                "a clone is made for the refusals");
        Node refused_hidden;
        Node hidden_source;
        verdict(tally,
                make(hidden_source, 1) && make_child(hidden_source, 0, 1, 1, true, refused_hidden),
                "a hidden node is made");
        verdict(tally, clone_of(refused_hidden, 0, 1, refused_clone) == Status::BadState,
                "a hidden node cannot be cloned");
        {
            Node misaligned;
            verdict(tally, clone_raw(origin, 1, PAGE_SIZE, misaligned) == Status::InvArgs,
                    "an offset that is not page aligned is refused");
            verdict(tally, clone_raw(origin, 0, 1, misaligned) == Status::InvArgs,
                    "and so is a size that is not");
        }

        // A clone of a node with nothing at all still gets a hidden node above,
        // because what the node writes later must not reach the clone.
        Node blank;
        Node blank_clone;
        verdict(tally, make(blank, 2) && clone_of(blank, 0, 2, blank_clone) == Status::Ok,
                "a node with no pages is cloned");
        auto const *const blank_hidden = parent_of(blank);
        verdict(tally,
                blank_hidden != nullptr && blank_hidden->is_hidden() &&
                    parent_of(blank_clone) == blank_hidden && pages_in(blank, blank_hidden) == 0 &&
                    children_in(blank, blank_hidden) == 2 && reads_zero_page(blank, 0) &&
                    reads_zero_page(blank_clone, 1),
                "it gets one, empty, and both read zero");

        // The write. A node that writes a page it sees only through the hidden
        // node above it gets a page of its own: a copy while someone else still
        // sees the page, the page itself when it is the last.
        Node w_origin;
        verdict(tally, make(w_origin, 3), "a node to write through is made");
        auto *const w0 = fill(w_origin, 0, 0xD0);
        auto *const w1 = fill(w_origin, 1, 0xD1);
        Node w_a;
        Node w_b;
        verdict(tally,
                w0 && w1 && clone_of(w_origin, 0, 3, w_a) == Status::Ok &&
                    clone_of(w_origin, 0, 3, w_b) == Status::Ok,
                "it holds two pages and has two clones");
        auto const *const w_hidden = parent_of(w_origin);
        verdict(tally,
                w_hidden != nullptr && pages_in(w_origin, w_hidden) == 2 &&
                    w0->share_count() == 2 && w1->share_count() == 2,
                "three nodes see each page, so each has a count of two");

        // The first writer of a page others still see gets a copy.
        auto const wrote_a = lookup(w_a, true, 0);
        verdict(tally,
                wrote_a && wrote_a->writable && wrote_a->page != w0 &&
                    holds_only(wrote_a->page, 0xD0) && nr_pages(w_a) == 1 &&
                    w0->share_count() == 1 && pages_in(w_origin, w_hidden) == 2,
                "the first writer of a shared page gets a copy of it, and the page stays");
        std::memset(bytes_of(wrote_a->page), 0xEE, PAGE_SIZE);
        verdict(tally,
                holds_only(w0, 0xD0) && reads_lenders_page(w_b, 0, w0) &&
                    reads_lenders_page(w_origin, 0, w0),
                "what it writes does not reach the page the others read");
        auto const again_a = lookup(w_a, false, 0);
        verdict(tally,
                again_a && again_a->page == wrote_a->page && again_a->writable &&
                    holds_only(again_a->page, 0xEE),
                "and it reads its own page afterwards, writable");

        // The second writer forks as well: one other viewer is still there.
        auto const wrote_b = lookup(w_b, true, 0);
        verdict(tally,
                wrote_b && wrote_b->page != w0 && wrote_b->page != wrote_a->page &&
                    holds_only(wrote_b->page, 0xD0) && w0->share_count() == 0 &&
                    pages_in(w_origin, w_hidden) == 2,
                "the second writer forks too, and the count is now zero");

        // The last node that sees the page takes it: nothing is copied.
        auto const wrote_o = lookup(w_origin, true, 0);
        verdict(tally,
                wrote_o && wrote_o->page == w0 && wrote_o->writable &&
                    pages_in(w_origin, w_hidden) == 1 && nr_pages(w_origin) == 1 &&
                    holds_only(w0, 0xD0),
                "the last node that sees a page takes the page itself, and the hidden node "
                "lets go of it");

        // The other order, on page 1: two forks and then the take.
        auto const o1w = lookup(w_origin, true, 1);
        auto const a1w = lookup(w_a, true, 1);
        auto const b1w = lookup(w_b, true, 1);
        verdict(tally,
                o1w && a1w && b1w && o1w->page != w1 && a1w->page != w1 && b1w->page == w1 &&
                    w1->share_count() == 0 && pages_in(w_origin, w_hidden) == 0 &&
                    holds_only(o1w->page, 0xD1) && holds_only(a1w->page, 0xD1) &&
                    holds_only(w1, 0xD1),
                "whoever writes last takes the page, whichever node that is");

        // A hole, and the zero tail, are allocated, not copied.
        auto const hole = lookup(w_a, true, 2);
        verdict(tally,
                hole && hole->writable && hole->page != mm::get_zero_page() &&
                    holds_only(hole->page, 0) && nr_pages(w_a) == 3,
                "a write to a hole gets a zeroed page of its own");
        auto const tail = lookup(longer, true, 4);
        verdict(tally,
                tail && tail->writable && tail->page != mm::get_zero_page() &&
                    holds_only(tail->page, 0) && nr_pages(longer) == 1,
                "and so does a write past the end of what the clone can see");

        // A page a node that is not hidden holds is only ever copied.
        auto const before_pages = nr_pages(parent);
        auto const under_plain = lookup(child, true, 0);
        verdict(tally,
                under_plain && under_plain->page != p0 && holds_only(under_plain->page, 0xA0) &&
                    nr_pages(parent) == before_pages && p0->share_count() == 0 &&
                    nr_pages(child) == 1,
                "a page held by a node that is not hidden is copied, never taken, and its count "
                "is left alone");

        // The new hidden node takes the node's place on its parent's list.
        Node top;
        Node under_top;
        Node under_top_clone;
        verdict(tally,
                make(top, 2) && make_child(top, 0, 2, 2, false, under_top) &&
                    children_in(top, top.as_ptr()) == 1,
                "a node under a parent is made");
        verdict(tally, clone_of(under_top, 0, 2, under_top_clone) == Status::Ok, "and cloned");
        auto const *const between = parent_of(under_top);
        verdict(tally,
                between != nullptr && between->is_hidden() &&
                    parent_of(under_top_clone) == between && children_in(top, top.as_ptr()) == 1 &&
                    children_in(top, between) == 2,
                "the hidden node stands between them, and the parent still lists one child");

        // The hidden node and its pages outlive every handle but one.
        origin = nullptr;
        clone1 = nullptr;
        clone2 = nullptr;
        window = nullptr;
        of_clone = nullptr;
        verdict(tally,
                reads_lenders_page(longer, 0, o0) && reads_lenders_page(longer, 1, o1) &&
                    holds_only(o0, 0xC0),
                "with every other handle dropped the last clone still reads the pages");

        // The last handle to the parent goes; the children keep the tree.
        Node orphan_parent;
        Node orphan;
        auto const made =
            make(orphan_parent, 1) && make_child(orphan_parent, 0, 1, 1, false, orphan);
        verdict(tally, made, "a parent and its child are made for the last check");
        if (made) {
            auto *const kept = fill(orphan_parent, 0, 0xB0);
            auto *const lock = &orphan->lock();
            orphan_parent = nullptr;
            verdict(
                tally,
                kept != nullptr && &orphan->lock() == lock && reads_lenders_page(orphan, 0, kept) &&
                    holds_only(kept, 0xB0),
                "a child whose parent handle is dropped still reads it, and still has its lock");
        }

        if (tally.failed != 0) {
            log::error("vmosnapshot.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmosnapshot.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmoSnapshotTest, "vmosnapshot.test", ours::vmosnapshot_test_command,
             "check what a VmCowPages under another one sees: lender's pages, zeros, its lock");
