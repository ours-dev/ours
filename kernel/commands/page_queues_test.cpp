/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `pagequeues.test`: every page a VMO holds is filed on its node's page queue
// for its kind, with a backlink to the node that holds it, and taken off when
// it leaves (.agents/tasks/page-reclaim, step 1).
//
//   - a page written into a hole of an anonymous VMO is anonymous zero-fork,
//     owned by the VMO's node at its index;
//   - cloned, the page moves to the hidden node and keeps its kind; written
//     by the clone, the clone's copy is anonymous and the clone's; written by
//     the original, which is then its last viewer, the page is the original's
//     again;
//   - a pinned VMO's pages are pinned;
//   - pages a page source supplied are reclaimable, owned by the VMO's node;
//   - once every VMO here is gone, every queue holds what it held before.
//
// Why a console command: the queues are the real nodes', filled by the real
// VMO paths.
#include <ours/mm/gaf.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page_splice_list.hpp>

#include <array>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using Kind = mm::PageQueues::PageKind;
    constexpr u64 P = PAGE_SIZE;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pagequeues.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pagequeues.test: FAIL  {}", name);
    }

    /// The page |vmo| sees at |offset|, and what its descriptor says about it.
    struct Filed {
        mm::VmPage *page = nullptr;
        Kind kind = Kind::None;
        void *owner = nullptr;
        PgOff index = 0;
    };

    // The analysis cannot tie `node->lock()` to the lock the cursor requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end, and the descriptor is read under it, as the queues require.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto filed_at(mm::VmObjectPaged &vmo, u64 const offset) -> Filed {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(offset, P);
        if (!cursor) {
            return {};
        }
        auto const found = cursor->maybe_page();
        if (found.is_none()) {
            return {};
        }
        auto *const page = found.value().page;
        return {.page = page,
                .kind = mm::PageQueues::kind_of(*page),
                .owner = page->get_object(),
                .index = page->get_pgoff()};
    }

    /// The node |vmo| sees its pages through, to compare a backlink with.
    auto node_of(mm::VmObjectPaged &vmo) -> void const * {
        return vmo.cow_pages().as_ptr();
    }

    /// Every queue's count on node 0, the one node of the default build.
    auto counts() -> std::array<usize, 5> {
        auto &queues = mm::PmNode::node(0)->page_queues();
        return {queues.count(Kind::None), queues.count(Kind::Pinned), queues.count(Kind::Anonymous),
                queues.count(Kind::AnonymousZeroFork), queues.count(Kind::Reclaimable)};
    }

    auto write_word(mm::VmObjectPaged &vmo, u64 const offset, u64 const value) -> bool {
        return vmo.write(&value, offset, sizeof(value)) == Status::Ok;
    }

    auto check_anonymous(Tally &tally) -> void {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::GAF_USER_VMO, 4 * P, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            verdict(tally, false, "an anonymous VMO is made");
            return;
        }
        auto const written = write_word(*vmo, P, 0x0123'4567'89ab'cdef);
        auto const first = filed_at(*vmo, P);
        verdict(tally,
                written && first.page && first.kind == Kind::AnonymousZeroFork &&
                    first.owner == node_of(*vmo) && first.index == 1,
                "a page written into a hole is anonymous zero-fork, the VMO's, at its index");

        ustl::Arc<mm::VmObjectPaged> clone;
        auto const cloned = vmo->create_clone(0, 4 * P, &clone) == Status::Ok;
        auto const hidden = filed_at(*vmo, P);
        verdict(tally,
                cloned && hidden.page == first.page && hidden.kind == Kind::AnonymousZeroFork &&
                    hidden.owner != node_of(*vmo) && hidden.owner != nullptr && hidden.index == 1,
                "cloned, the page moves to the hidden node and keeps its kind");

        auto const clone_wrote = cloned && write_word(*clone, P, 0x5eed);
        auto const copy = filed_at(*clone, P);
        verdict(tally,
                clone_wrote && copy.page && copy.page != first.page &&
                    copy.kind == Kind::Anonymous && copy.owner == node_of(*clone) &&
                    copy.index == 1,
                "the clone's copy of the page is anonymous and the clone's");

        auto const original_wrote = write_word(*vmo, P, 0xfeed);
        auto const taken = filed_at(*vmo, P);
        verdict(tally,
                original_wrote && taken.page == first.page &&
                    taken.kind == Kind::AnonymousZeroFork && taken.owner == node_of(*vmo) &&
                    taken.index == 1,
                "the last viewer takes the hidden node's page, which is its own again");
    }

    auto check_pinned(Tally &tally) -> void {
        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const made = mm::VmObjectPaged::create(mm::GAF_KERNEL, 2 * P, mm::VmoFlags::Pinned,
                                                    &vmo) == Status::Ok;
        auto page = Filed{};
        if (made) {
            page = filed_at(*vmo, P);
        }
        verdict(tally,
                made && page.page && page.kind == Kind::Pinned && page.owner == node_of(*vmo) &&
                    page.index == 1,
                "a pinned VMO's pages are pinned");
    }

    /// A provider nobody asks: the test supplies the pages itself.
    class SilentProvider final: public mm::PageProvider {
        auto send_async_request(mm::PageRequest *) -> void override {}
        auto clear_async_request(mm::PageRequest *) -> void override {}
        auto swap_async_request(mm::PageRequest *, mm::PageRequest *) -> void override {}
        auto wait_on_event(sync::Event *, bool) -> Status override {
            return Status::TimeOut;
        }
        auto on_detach() -> void override {}
        auto on_close() -> void override {}
    };

    auto check_reclaimable(Tally &tally) -> void {
        auto *const provider = new (mm::GAF_KERNEL) SilentProvider();
        if (!provider) {
            verdict(tally, false, "a page provider is made");
            return;
        }
        ustl::Arc<mm::PageSource> source;
        ustl::Arc<mm::VmObjectPaged> vmo;
        auto made = mm::PageSource::create(ustl::Arc<mm::PageProvider>::adopt(provider), &source) ==
                        Status::Ok &&
                    mm::VmObjectPaged::with_source(source, mm::GAF_USER, 2 * P, mm::VmoFlags::Lazy,
                                                   &vmo) == Status::Ok;

        auto supplied = false;
        if (made) {
            mm::VmPageSpliceList pages(2);
            supplied = pages.reserve(2) == Status::Ok;
            for (usize i = 0; supplied && i < 2; ++i) {
                auto *const frame = mm::alloc_frames(mm::GAF_KERNEL | mm::Gaf::Zero);
                supplied = frame && pages.append_page(i, frame) == Status::Ok;
            }
            supplied = supplied &&
                       vmo->supply_pages(0, 2 * P, &pages, mm::VmObject::SupplyOption::Default) ==
                           Status::Ok;
        }
        auto page = Filed{};
        if (supplied) {
            page = filed_at(*vmo, P);
        }
        verdict(tally,
                made && supplied && page.page && page.kind == Kind::Reclaimable &&
                    page.owner == node_of(*vmo) && page.index == 1,
                "a page a page source supplied is reclaimable, the VMO's, at its index");
    }

    auto pagequeues_test_command(int, char **) -> int {
        Tally tally;
        auto const before = counts();
        check_anonymous(tally);
        check_pinned(tally);
        check_reclaimable(tally);
        auto const after = counts();
        verdict(tally, before == after,
                "with every VMO here gone, every queue holds what it held before");
        if (before != after) {
            log::error("pagequeues.test: pinned {} -> {}, anonymous {} -> {}, zero-fork {} -> {}, "
                       "reclaimable {} -> {}",
                       before[1], after[1], before[2], after[2], before[3], after[3], before[4],
                       after[4]);
        }

        if (tally.failed != 0) {
            log::error("pagequeues.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pagequeues.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PageQueuesTest, "pagequeues.test", ours::pagequeues_test_command,
             "check that VMO pages are filed on their node's page queues by kind, and taken off");
