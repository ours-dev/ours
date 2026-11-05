/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmoPin.PinRangeAndUnpinRange: VmObjectPaged::pin_range() and unpin_range(),
// the pin a device's address depends on.
//
// Why an in-kernel test: the pin is not reachable from a program yet (the call
// that will use it, bti_pin, comes later), and what it promises is about the
// pages' state -- their count, the queue they are on, what the node refuses --
// which no program can read.
//
// On a lazy VMO of four pages, nothing committed:
//
//   - pinning pages 1 and 2 commits them, counts one pin on each and files them
//     on the pinned queue; pages 0 and 3 are not touched;
//   - a clone of the object is BadState while any page is pinned, and take_pages()
//     of a range holding one is BadState, where one of a range holding none is
//     not refused for that;
//   - the reclaimer's second route to a page, the zero dedup, leaves a pinned
//     zero page alone, where it frees an identical page that is not pinned;
//   - a page pinned twice stays pinned after one unpin and is let go by the
//     second, and goes back on a queue the reclaimer reads;
//   - unpinning what holds no pin is BadState and undoes nothing;
//   - the count stops at 255 (NoResource), and a pin that fails part way leaves
//     no pin of its own behind;
//   - the arguments: unaligned or empty is InvArgs, past the end OutOfRange;
//   - once nothing is pinned a clone is made.
#include <ours/mm/gaf.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>

#include <ktest/ktest.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize P = PAGE_SIZE;
    constexpr usize PAGES = 4;

    /// What a node holds at one page: the page, and what it says.
    struct Filed {
        mm::VmPage *page = nullptr;
        usize pins = 0;
        mm::PageQueues::PageKind kind = mm::PageQueues::PageKind::None;
    };

    // The analysis cannot tie `node->lock()` to the lock the cursor requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto filed_at(mm::VmObjectPaged &vmo, usize const index) -> Filed {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(index * P, P);
        if (!cursor) {
            return {};
        }
        // Looking is not using.
        cursor->disable_mark_accessed();
        auto const found = cursor->maybe_page();
        if (found.is_none()) {
            return {};
        }
        auto *const page = found.value().page;
        return {.page = page,
                .pins = page->pinned_count(),
                .kind = mm::PmNode::node(page->nid())->page_queues().kind_of(*page)};
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pinned(mm::VmObjectPaged &vmo) -> usize {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        return node->nr_pinned_pages_locked();
    }

    using Kind = mm::PageQueues::PageKind;

    KTEST(VmoPin, PinRangeAndUnpinRange) {
        ustl::Arc<mm::VmObjectPaged> vmo;
        ASSERT_OK(mm::VmObjectPaged::create(mm::GAF_USER, PAGES * P, mm::VmoFlags::Lazy, &vmo),
                  "a lazy VMO is made");

        EXPECT_TRUE(!filed_at(*vmo, 0).page && !filed_at(*vmo, 1).page,
                    "a lazy VMO starts with no page");

        // ---- pin two pages
        EXPECT_OK(vmo->pin_range(P, 2 * P), "pin_range(pages 1 and 2) is Ok");
        auto const one = filed_at(*vmo, 1);
        auto const two = filed_at(*vmo, 2);
        EXPECT_TRUE(one.page && two.page && one.pins == 1 && two.pins == 1 &&
                        one.kind == Kind::Pinned && two.kind == Kind::Pinned,
                    "each page is committed, holds one pin, and is on the pinned queue");
        EXPECT_TRUE(!filed_at(*vmo, 0).page && !filed_at(*vmo, 3).page,
                    "the pages beside the range are not committed");
        EXPECT_EQ(nr_pinned(*vmo), usize{2}, "the node counts two pinned pages");

        // ---- what a pin forbids
        ustl::Arc<mm::VmObjectPaged> clone;
        EXPECT_EQ(vmo->create_clone(0, PAGES * P, &clone), Status::BadState,
                  "a clone is BadState while a page is pinned");
        {
            mm::VmPageSpliceList over_pin(2);
            EXPECT_EQ(vmo->take_pages(P, 2 * P, &over_pin), Status::BadState,
                      "take_pages() over pinned pages is BadState");
        }
        {
            // A range with no pin in it is not refused for one. Nothing is
            // committed there, so the call has nothing to take, and it succeeds.
            mm::VmPageSpliceList beside(1);
            EXPECT_OK(vmo->take_pages(3 * P, P, &beside),
                      "take_pages() over a page with no pin is not refused");
        }

        // ---- the zero dedup: the same page, pinned and not
        EXPECT_OK(vmo->commit_range(0, P, mm::VmObject::CommitOptions::None),
                  "page 0 is committed, and holds no pin");
        auto const loose = filed_at(*vmo, 0);
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo->cow_pages());
        EXPECT_TRUE(loose.page && loose.kind == Kind::AnonymousZeroFork,
                    "a page committed in place of zero is on the zero-fork queue the dedup "
                    "reads");
        EXPECT_TRUE(one.page &&
                        node->dedup_zero_page(1, one.page) == mm::VmCowPages::ReclaimResult::Kept &&
                        filed_at(*vmo, 1).page == one.page,
                    "the zero dedup keeps a zero page that is pinned");
        EXPECT_TRUE(loose.page &&
                        node->dedup_zero_page(0, loose.page) ==
                            mm::VmCowPages::ReclaimResult::Evicted &&
                        !filed_at(*vmo, 0).page,
                    "and frees an identical zero page that is not");

        // ---- counted
        EXPECT_TRUE(vmo->pin_range(P, P) == Status::Ok && filed_at(*vmo, 1).pins == 2,
                    "a second pin of page 1 makes two");
        EXPECT_TRUE(vmo->unpin_range(P, P) == Status::Ok && filed_at(*vmo, 1).pins == 1 &&
                        filed_at(*vmo, 1).kind == Kind::Pinned,
                    "one unpin leaves it pinned");

        // ---- unmatched
        EXPECT_TRUE(vmo->unpin_range(0, 3 * P) == Status::BadState && filed_at(*vmo, 1).pins == 1 &&
                        filed_at(*vmo, 2).pins == 1,
                    "unpinning a range with a page that holds no pin is BadState, and undoes "
                    "none");

        // ---- let go
        EXPECT_OK(vmo->unpin_range(P, 2 * P), "unpin_range(pages 1 and 2) is Ok");
        EXPECT_TRUE(filed_at(*vmo, 1).pins == 0 && filed_at(*vmo, 1).kind == Kind::Anonymous &&
                        filed_at(*vmo, 2).pins == 0 && filed_at(*vmo, 2).kind == Kind::Anonymous &&
                        nr_pinned(*vmo) == 0,
                    "the last unpin files each page anonymous again, and the node counts none");
        EXPECT_EQ(vmo->unpin_range(P, P), Status::BadState,
                  "an unpin of a page that holds none is BadState");

        // ---- the limit, and a pin that fails part way
        auto limit_ok = true;
        for (usize pin = 0; pin < 255; ++pin) {
            limit_ok = limit_ok && vmo->pin_range(3 * P, P) == Status::Ok;
        }
        EXPECT_TRUE(limit_ok && filed_at(*vmo, 3).pins == 255, "a page takes 255 pins");
        EXPECT_EQ(vmo->pin_range(3 * P, P), Status::NoResource,
                  "the 256th pin of a page is NoResource");
        EXPECT_TRUE(vmo->pin_range(2 * P, 2 * P) == Status::NoResource &&
                        filed_at(*vmo, 2).pins == 0 && filed_at(*vmo, 3).pins == 255,
                    "a pin that fails at its second page leaves no pin on its first");
        auto released = true;
        for (usize pin = 0; pin < 255; ++pin) {
            released = released && vmo->unpin_range(3 * P, P) == Status::Ok;
        }
        EXPECT_TRUE(released && filed_at(*vmo, 3).pins == 0 && nr_pinned(*vmo) == 0,
                    "255 unpins let it go");

        // ---- arguments
        EXPECT_EQ(vmo->pin_range(1, P), Status::InvArgs, "an unaligned offset is InvArgs");
        EXPECT_EQ(vmo->pin_range(0, P + 1), Status::InvArgs, "an unaligned size is InvArgs");
        EXPECT_EQ(vmo->pin_range(0, 0), Status::InvArgs, "an empty range is InvArgs");
        EXPECT_EQ(vmo->pin_range(0, (PAGES + 1) * P), Status::OutOfRange,
                  "a range past the end is OutOfRange");
        EXPECT_EQ(vmo->pin_range(PAGES * P, P), Status::OutOfRange,
                  "a range that starts at the end is OutOfRange");

        // ---- after
        EXPECT_OK(vmo->create_clone(0, PAGES * P, &clone),
                  "a clone is made once nothing is pinned");
    }

} // namespace
} // namespace ours
