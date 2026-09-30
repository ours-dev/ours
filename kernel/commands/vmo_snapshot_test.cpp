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
        return VmCowPages::create_child(parent, parent_page * PAGE_SIZE, limit_pages * PAGE_SIZE,
                                        pages * PAGE_SIZE, hidden, &out) == Status::Ok;
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
                VmCowPages::create_child(parent, 1, PAGE_SIZE, PAGE_SIZE, false, &refused) ==
                    Status::InvArgs,
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
