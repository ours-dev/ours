/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmocontig.test`: mm::VmObjectPaged::create_contiguous(), the memory a device
// is given one address of.
//
// Why a console command: the call that reaches it from a program
// (vmo_create_contiguous) cannot see a frame's address, the queue a page is
// on, or the allocator's count of free frames, and those are what it promises.
//
// It makes VMOs of 1, 3 and 4 pages, one of 2 pages on 64 KiB, one page on
// 2 MiB, and the largest block (4 MiB), and checks of each:
//
//   - the size is rounded up to a page, and every page is there from the start;
//   - the pages are one run (lookup_contiguous()), start on the alignment, and
//     each is on the pinned queue, which the reclaimer never takes from;
//   - they are zero (a fresh allocation: that the zero survives frames that
//     held something is the allocator's Gaf::Zero, which this does not retest);
//   - pin_range() and unpin_range() leave them on the pinned queue, and a BTI
//     pin with Contiguous is Ok and answers the first page's address;
//
// and refuses what cannot be: a size past the largest block or an alignment past
// it (NoMemory), an alignment below a page or past a shift, an empty size and a
// lazy VMO (InvArgs), a size no VMO can be (OutOfRange). Making and dropping a
// VMO a hundred times leaves the allocator with the frames it had, which is what
// shows that the tail of a block is given back.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/iommu_dispatcher.hpp>
#include <ours/object/pmt_dispatcher.hpp>

#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize P = PAGE_SIZE;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("vmocontig.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmocontig.test: FAIL  {}", name);
    }

    using Kind = mm::PageQueues::PageKind;

    /// The queue the page at |index| of |vmo| is on, None if it has no page.
    // The analysis cannot tie `node->lock()` to the lock the cursor requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto kind_at(mm::VmObjectPaged &vmo, usize const index) -> Kind {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(index * P, P);
        // Looking is not using.
        cursor.disable_mark_accessed();
        auto const found = cursor.maybe_page();
        if (found.is_none()) {
            return Kind::None;
        }
        auto *const page = found.value().page;
        return mm::PmNode::node(page->nid())->page_queues().kind_of(*page);
    }

    /// Where page |index| of |vmo| is, 0 if it is not there.
    auto phys_of(mm::VmObjectPaged &vmo, usize const index) -> PhysAddr {
        auto pa = PhysAddr{0};
        if (vmo.lookup_contiguous(index * P, P, &pa) != Status::Ok) {
            return PhysAddr{0};
        }
        return pa;
    }

    /// Whether every page of the first |pages| of |vmo| is on the pinned queue.
    auto all_pinned_queue(mm::VmObjectPaged &vmo, usize const pages) -> bool {
        for (usize index = 0; index < pages; ++index) {
            if (kind_at(vmo, index) != Kind::Pinned) {
                return false;
            }
        }
        return true;
    }

    /// Whether the first |pages| pages of |vmo| are all zero, read through the
    /// kernel's view of physical memory.
    auto all_zero(mm::VmObjectPaged &vmo, usize const pages) -> bool {
        for (usize index = 0; index < pages; ++index) {
            auto const *const bytes = mm::PhysMap::phys_to_virt<u8 const>(phys_of(vmo, index));
            for (usize at = 0; at < P; ++at) {
                if (bytes[at] != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    auto make(usize const size, usize const alignment_log2, ustl::Arc<mm::VmObjectPaged> *out)
        -> Status {
        return mm::VmObjectPaged::create_contiguous(mm::ALLOC_USER, size, alignment_log2,
                                                    mm::VmoFlags::Pinned, out);
    }

    auto vmocontig_test_command(int, char **) -> int {
        Tally tally;
        ustl::Arc<mm::VmObjectPaged> vmo;

        // ---- one page, and the size rounded up
        verdict(tally,
                make(1, 0, &vmo) == Status::Ok && vmo->size() == P &&
                    phys_of(*vmo, 0) != PhysAddr{0} && all_pinned_queue(*vmo, 1) &&
                    all_zero(*vmo, 1),
                "one byte is one page, there from the start, on the pinned queue, and zero");
        verdict(tally,
                make(P + 1, 0, &vmo) == Status::Ok && vmo->size() == 2 * P &&
                    phys_of(*vmo, 1) == phys_of(*vmo, 0) + P,
                "a size of a page and a byte is two pages, one after the other");

        // ---- three pages: a block of four, the fourth given back
        verdict(tally, make(3 * P, 0, &vmo) == Status::Ok && vmo->size() == 3 * P,
                "three pages are made");
        auto first = PhysAddr{0};
        verdict(tally,
                vmo->lookup_contiguous(0, 3 * P, &first) == Status::Ok &&
                    std::to_underlying(first) % (4 * P) == 0 && first == phys_of(*vmo, 0) &&
                    all_pinned_queue(*vmo, 3) && all_zero(*vmo, 3),
                "they are one run on the block's own alignment, on the pinned queue, and zero");
        verdict(tally, kind_at(*vmo, 3) == Kind::None, "there is no fourth page");

        // ---- alignment
        verdict(tally,
                make(2 * P, 16, &vmo) == Status::Ok &&
                    std::to_underlying(phys_of(*vmo, 0)) % (usize{1} << 16) == 0 &&
                    vmo->lookup_contiguous(0, 2 * P, &first) == Status::Ok,
                "two pages on 64 KiB are one run starting on a multiple of 64 KiB");
        verdict(tally,
                make(P, 21, &vmo) == Status::Ok &&
                    std::to_underlying(phys_of(*vmo, 0)) % (usize{1} << 21) == 0,
                "one page on 2 MiB starts on a multiple of 2 MiB");

        // ---- the largest block
        constexpr auto MOST = usize{1} << MAX_FRAME_ORDER;
        verdict(tally,
                make(MOST * P, 0, &vmo) == Status::Ok && vmo->size() == MOST * P &&
                    vmo->lookup_contiguous(0, MOST * P, &first) == Status::Ok &&
                    std::to_underlying(first) % (MOST * P) == 0 && all_pinned_queue(*vmo, MOST) &&
                    all_zero(*vmo, MOST),
                "the largest block is made: all its pages one run, on its alignment, pinned queue, "
                "zero");
        vmo = {};

        // ---- what cannot be
        ustl::Arc<mm::VmObjectPaged> none;
        verdict(tally,
                make((MOST + 1) * P, 0, &none) == Status::NoMemory && !none &&
                    make(P, 12 + MAX_FRAME_ORDER + 1, &none) == Status::NoMemory &&
                    make(P, 40, &none) == Status::NoMemory,
                "a size or an alignment past the largest block is NoMemory");
        verdict(tally,
                make(0, 0, &none) == Status::InvArgs && make(P, 5, &none) == Status::InvArgs &&
                    make(P, 64, &none) == Status::InvArgs && make(P, 200, &none) == Status::InvArgs,
                "an empty size, an alignment below a page and one that is not a shift are InvArgs");
        verdict(tally,
                mm::VmObjectPaged::create_contiguous(mm::ALLOC_USER, P, 0, mm::VmoFlags::Lazy,
                                                     &none) == Status::InvArgs &&
                    mm::VmObjectPaged::create_contiguous(
                        mm::ALLOC_USER, P, 0, mm::VmoFlags::LargePages, &none) == Status::InvArgs,
                "a lazy or a large-page VMO is InvArgs");
        verdict(tally, make(~usize{0}, 0, &none) == Status::OutOfRange && !none,
                "a size no VMO can be is OutOfRange");

        // ---- what moves a page is refused
        {
            ustl::Arc<mm::VmObjectPaged> clone;
            verdict(tally,
                    make(2 * P, 0, &vmo) == Status::Ok &&
                        vmo->create_clone(0, 2 * P, &clone) == Status::BadState && !clone,
                    "a snapshot of the memory is refused: it would move the pages the device has");
        }

        // ---- the pinned queue through a pin
        verdict(tally,
                make(3 * P, 0, &vmo) == Status::Ok && vmo->pin_range(0, 3 * P) == Status::Ok &&
                    vmo->unpin_range(0, 3 * P) == Status::Ok && all_pinned_queue(*vmo, 3),
                "after pin_range and unpin_range the pages are still on the pinned queue");

        // ---- a BTI pin with Contiguous
        {
            object::KernelHandle<object::IommuDispatcher> iommu;
            object::KernelHandle<object::BtiDispatcher> bti;
            auto rights = Rights::None;
            auto const made =
                object::IommuDispatcher::create(IommuType::Stub, 0, iommu, rights) == Status::Ok &&
                object::BtiDispatcher::create(iommu.dispatcher(), 1, bti, rights) == Status::Ok;
            object::KernelHandle<object::PmtDispatcher> pin;
            auto pmt_rights = Rights::None;
            auto const options =
                BtiPinOptions::PermRead | BtiPinOptions::PermWrite | BtiPinOptions::Contiguous;
            auto const pinned =
                made && vmo &&
                bti.dispatcher()->pin(vmo, 0, 3 * P, options, pin, pmt_rights) == Status::Ok;
            std::array<PhysAddr, 1> address{};
            verdict(tally,
                    pinned && pin.dispatcher()->query_pieces(3 * P, 0, address) == Status::Ok &&
                        address[0] == phys_of(*vmo, 0),
                    "a BTI pin with Contiguous over the three pages is Ok, and the one address is "
                    "the first page's");
            if (pin.dispatcher()) {
                pin.dispatcher()->unpin();
            }
        }
        vmo = {};

        // ---- nothing leaks
        {
            auto const before = mm::free_frame_count();
            auto all_made = true;
            for (usize round = 0; round < 100; ++round) {
                ustl::Arc<mm::VmObjectPaged> temporary;
                all_made = all_made && make(3 * P, 16, &temporary) == Status::Ok;
            }
            auto const after = mm::free_frame_count();
            // The per-CPU caches and other threads move the count by a few
            // pages; one frame leaked in each of the hundred rounds is more.
            auto lost = usize{0};
            if (before > after) {
                lost = before - after;
            }
            verdict(tally, all_made && lost <= 32,
                    "a hundred VMOs made and dropped leave the allocator its frames");
            log::info("vmocontig.test: {} frames before, {} after", before, after);
        }

        if (tally.failed != 0) {
            log::error("vmocontig.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmocontig.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmoContigTest, "vmocontig.test", ours::vmocontig_test_command,
             "check create_contiguous(): one run, aligned, zero, on the pinned queue, the "
             "largest block, what it refuses, and that the tail of a block is given back");
