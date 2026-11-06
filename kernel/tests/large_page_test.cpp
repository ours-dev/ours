/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// LargePage.LeavesSplitWithSetAsideTables: 2 MiB leaves in a user address
// space, and the tables set aside to split them (.agents/tasks/large-pages).
//
// It maps a physically contiguous, 2 MiB aligned block straight through the
// address space's page table, as one leaf, and then changes parts of it:
//
//   - the leaf takes one table set aside (PageTableDeposits::nr_held());
//   - an unmap of one page splits it: the page is gone, its neighbours are
//     4 KiB entries to the same frames, and the table set aside is used;
//   - a protect of another page splits nothing more and narrows only that page;
//   - the split range, mapped again, stays small entries: unmapping never
//     frees the table it was split into;
//   - a leaf mapped and unmapped whole gives its table back;
//   - when the address space goes, every table set aside has come back,
//     including the one of a leaf still mapped then.
//
// And the VMO side: a node that takes 2 MiB blocks (VmCowPages::Flags::
// LargeBlocks) fills a hole with the 2 MiB aligned, contiguous block it lies
// in, made of order-0 frames; a hole where no block fits, or in a range that
// holds something already, or in a node that does not take blocks, gets one
// page.
//
// And the mappings: a fault in a large-page object maps the 2 MiB around the
// page as one leaf, writable; a window with one page read-only, an object
// offset not 2 MiB aligned, and a window split before, each get small
// entries; a protect of one page of a leaf splits it; a populate mapping maps
// whole blocks as leaves; a clone's read maps the shared block as one
// read-only leaf, and a write to one page of it splits the leaf, copies that
// page and leaves its neighbours read-only on the block; a window whose
// first page the clone took for itself, the rest still shared, is no block;
// and a mapping the VMM places starts where its object offset falls in a
// block, so its whole blocks line up with its windows. An unmap of one page
// of a leaf through the node, the walk take_pages() makes over every mapping
// of the pages, splits the leaf and takes that page alone.
//
// Why an in-kernel test: the deposits live in the descriptors of real page
// tables, built by the real allocator under the page table's lock, which the
// host test of the paging engine stands in for with a pool.
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/page_table_deposits.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize LARGE = 2_mb;
    constexpr usize ORDER = 9;
    static_assert((usize{1} << ORDER) * PAGE_SIZE == LARGE);

    /// What |va| translates to in |aspace|.
    struct Translation {
        bool present;
        PhysAddr pa;
        mm::MmuFlags flags;
        usize size;
    };

    auto translate(mm::VmAspace &aspace, VirtAddr const va) -> Translation {
        Translation result{};
        result.present =
            aspace.arch_aspace().query(va, &result.pa, &result.flags, &result.size) == Status::Ok;
        return result;
    }

    auto is_small_to(mm::VmAspace &aspace, VirtAddr const va, PhysAddr const pa) -> bool {
        auto const t = translate(aspace, va);
        return t.present && t.size == PAGE_SIZE && t.pa == pa;
    }

    using Node = ustl::Arc<mm::VmCowPages>;
    constexpr usize PAGES_PER_BLOCK = LARGE / PAGE_SIZE;

    // The analysis cannot tie `node->lock()` to the `m_lock` that lookup_cursor_locked()
    // and the _locked calls require: it sees two expressions through two calls
    // of Arc::operator->(). The lock is held from the guard to the end.

    /// The page a read or a write of page |index| of |node| answers, as a
    /// fault would ask for it.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto touch(Node &node, usize const index, bool const write) -> mm::VmPage * {
        Guard guard{node->lock()};
        auto cursor = node->lookup_cursor_locked(index * PAGE_SIZE, PAGE_SIZE);
        mm::MultiPageRequest request;
        auto found = cursor.require_page(write, 1, &request);
        if (!found) {
            return nullptr;
        }
        return found->page;
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pages(Node &node) -> usize {
        Guard guard{node->lock()};
        return node->nr_pages_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto add_marker(Node &node, usize const index) -> Status {
        Guard guard{node->lock()};
        return node->add_zero_marker_locked(index);
    }

    /// Whether pages [first, first + 512) of |node| are one 2 MiB block: 2 MiB
    /// aligned, physically contiguous, each a frame of order 0.
    auto is_block(Node &node, usize const first) -> bool {
        auto *const head = touch(node, first, false);
        if (head == nullptr) {
            return false;
        }
        auto const base = mm::frame_to_phys(head);
        if (std::to_underlying(base) % LARGE != 0) {
            return false;
        }
        for (usize i = 0; i < PAGES_PER_BLOCK; ++i) {
            auto *const page = touch(node, first + i, false);
            if (page == nullptr || mm::frame_to_phys(page) != base + i * PAGE_SIZE ||
                page->order() != 0) {
                return false;
            }
        }
        return true;
    }

    auto check_blocks() -> void {
        constexpr auto PER = PAGES_PER_BLOCK;

        // Two blocks and three pages past them.
        Node node;
        if (mm::VmCowPages::create(mm::ALLOC_USER, 2 * LARGE + 3 * PAGE_SIZE,
                                   mm::VmCowPages::Flags::LargeBlocks, &node) != Status::Ok) {
            FAIL("allocate a node that takes blocks");
        }
        auto *const written = touch(node, 5, true);
        EXPECT_TRUE(
            written != nullptr && nr_pages(node) == PER && is_block(node, 0) &&
                touch(node, 5, false) == written,
            "a write to page 5 fills its 2 MiB block: 512 pages, 2 MiB aligned, contiguous, "
            "each of order 0");
        EXPECT_TRUE(touch(node, PER + 3, false) != nullptr && nr_pages(node) == 2 * PER &&
                        is_block(node, PER),
                    "a read of the second 2 MiB fills a block of its own");
        EXPECT_TRUE(touch(node, 2 * PER, true) != nullptr && nr_pages(node) == 2 * PER + 1,
                    "a hole in the tail, where no block fits, gets one page");

        Node marked;
        EXPECT_TRUE(mm::VmCowPages::create(mm::ALLOC_USER, LARGE,
                                           mm::VmCowPages::Flags::LargeBlocks,
                                           &marked) == Status::Ok &&
                        add_marker(marked, 10) == Status::Ok && touch(marked, 3, true) != nullptr &&
                        nr_pages(marked) == 1,
                    "a hole in a block's range that holds a zero marker gets one page");

        Node plain;
        EXPECT_TRUE(mm::VmCowPages::create(mm::ALLOC_USER, LARGE, &plain) == Status::Ok &&
                        touch(plain, 3, true) != nullptr && nr_pages(plain) == 1,
                    "a node that does not take blocks gets one page");

#if MM_TEST_ALLOC_FAILURE
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        for (auto const site: {AllocationSite::Frame, AllocationSite::Collection}) {
            Node fallback;
            if (mm::VmCowPages::create(mm::ALLOC_USER, 2 * LARGE,
                                       mm::VmCowPages::Flags::LargeBlocks,
                                       &fallback) != Status::Ok) {
                FAIL("allocate a node for block allocation failure");
            }
            mm::VmPage *page;
            bool triggered;
            {
                // Skip the first tree allocation to fail after some pages were inserted.
                AllocationFailure failure(site, site == AllocationSite::Collection ? 1 : 0);
                page = touch(fallback, 3, true);
                triggered = failure.triggered();
            }
            EXPECT_TRUE(triggered && page && page->order() == 0 && nr_pages(fallback) == 1,
                        "an interrupted large block leaves only the single-page fallback");
            EXPECT_TRUE(is_block(fallback, PER) && nr_pages(fallback) == PER + 1,
                        "another block can still be filled after the failure");
        }
#endif
    }

    // Puts the calling thread on |next| and loads its page tables, as
    // vmosnapshot.test does, so that soft_fault() resolves a fault against the
    // test's address space. Preemption is off across the two steps: the
    // scheduler loads the page tables from the thread's address space on
    // every switch.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    auto make_large(usize const size, ustl::Arc<mm::VmObjectPaged> *const out) -> bool {
        return mm::VmObjectPaged::create(mm::ALLOC_USER, size,
                                         mm::VmoFlags::LargePages | mm::VmoFlags::Lazy,
                                         out) == Status::Ok;
    }

    /// Takes a fault at |va| and answers what |va| translates to after it; an
    /// absent Translation when the fault failed.
    auto fault_at(mm::VmAspace &aspace, VirtAddr const va, mm::VmFault::Cause const cause)
        -> Translation {
        if (task::Thread::Current::soft_fault(va, cause) != Status::Ok) {
            return Translation{};
        }
        return translate(aspace, va);
    }

    /// Unmaps [offset, offset + len) of |vmo|'s pages from every mapping of
    /// every object over them: the walk take_pages() makes.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto unmap_through_node(mm::VmObjectPaged const &vmo, usize const offset, usize const len)
        -> void {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        node->range_change_locked(offset, len, mm::RangeOp::Unmap);
    }

    /// A large-page object of |size| bytes, mapped read-write at |va| from
    /// |offset| on, into |out|; null when either step failed.
    auto map_large(mm::VmArea &root, VirtAddr const va, usize const size, u64 const offset,
                   mm::MapOptions const option, ustl::Arc<mm::VmObjectPaged> *const out)
        -> ustl::Arc<mm::VmMapping> {
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        if (!make_large(size + offset, out)) {
            return {};
        }
        auto mapped = root.map_vmo(va, size, offset, rw, mm::VmaFlags::PermMask, *out, option,
                                   "largepage.test");
        if (mapped.is_err()) {
            return {};
        }
        return mapped.unwrap();
    }

    auto check_mappings() -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "largepage.test mappings", &aspace);
            status != Status::Ok) {
            FAIL("allocate an address space for the mapping checks");
        }
        auto *const previous = attach(aspace.as_ptr_mut());
        auto &root = *aspace->root_vma();
        constexpr auto WRITE = mm::VmFault::Cause::Write;
        constexpr auto READ = mm::VmFault::Cause::Absent;
        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto const at = [](usize const megabytes) {
            return ustl::round_up(USER_ASPACE_BASE + megabytes * 1_mb, LARGE);
        };
        auto const writable = [](Translation const &t) {
            return !!(t.flags & mm::MmuFlags::Writable);
        };

        // A fault maps the whole window as one writable leaf; a window with one
        // page read-only gets one small entry; a leaf protected in part splits.
        ustl::Arc<mm::VmObjectPaged> plain;
        auto const a = at(128);
        auto mapping = map_large(root, a, 2 * LARGE, 0, mm::MapOptions::Lazy, &plain);
        EXPECT_TRUE(mapping, "a large-page object is mapped");
        if (mapping) {
            auto const leaf = fault_at(*aspace, a + 5 * PAGE_SIZE, WRITE);
            EXPECT_TRUE(leaf.present && leaf.size == LARGE && writable(leaf) &&
                            std::to_underlying((leaf.pa - 5 * PAGE_SIZE)) % LARGE == 0,
                        "a write fault in a large-page object maps its 2 MiB as one writable leaf");

            auto const second = a + LARGE;
            auto small = Translation{};
            if (mapping->protect(second + 6 * PAGE_SIZE, PAGE_SIZE, ro) == Status::Ok) {
                small = fault_at(*aspace, second + 5 * PAGE_SIZE, WRITE);
            }
            EXPECT_TRUE(small.present && small.size == PAGE_SIZE &&
                            !translate(*aspace, second + 4 * PAGE_SIZE).present,
                        "with one page of the window read-only, a fault maps only its own page");

            auto const split = mapping->protect(a + 7 * PAGE_SIZE, PAGE_SIZE, ro) == Status::Ok;
            auto const seventh = translate(*aspace, a + 7 * PAGE_SIZE);
            auto const eighth = translate(*aspace, a + 8 * PAGE_SIZE);
            EXPECT_TRUE(
                split && seventh.present && seventh.size == PAGE_SIZE && !writable(seventh) &&
                    eighth.present && eighth.size == PAGE_SIZE && writable(eighth),
                "a protect of one page of the leaf splits it, and only that page loses Write");
        }

        // An object offset that is not 2 MiB aligned never lines a block up
        // with a window.
        ustl::Arc<mm::VmObjectPaged> shifted;
        auto const b = at(256);
        auto off = Translation{};
        if (map_large(root, b, 2 * LARGE, PAGE_SIZE, mm::MapOptions::Lazy, &shifted)) {
            off = fault_at(*aspace, b + 5 * PAGE_SIZE, WRITE);
        }
        EXPECT_TRUE(off.present && off.size == PAGE_SIZE,
                    "an object offset 4 KiB off a 2 MiB boundary gets small entries");

        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;

        // A populate mapping maps whole blocks as leaves. A populate maps only
        // what the object has, so the object is made with its blocks (no Lazy).
        ustl::Arc<mm::VmObjectPaged> populated;
        auto const c = at(384);
        auto const populate =
            mm::VmObjectPaged::create(mm::ALLOC_USER, 2 * LARGE, mm::VmoFlags::LargePages,
                                      &populated) == Status::Ok &&
            root.map_vmo(c, 2 * LARGE, 0, rw, mm::VmaFlags::PermMask, populated,
                         mm::MapOptions::Resident, "largepage.test populate")
                .is_ok();
        auto const p0 = translate(*aspace, c);
        auto const p1 = translate(*aspace, c + LARGE + 9 * PAGE_SIZE);
        EXPECT_TRUE(populate && p0.present && p0.size == LARGE && p1.present && p1.size == LARGE,
                    "a populate mapping maps each 2 MiB block as one leaf");

        // Split by an unmap of one page, the window's protection left as it is:
        // its entry holds a table now, so a fault there maps one page, never a
        // leaf over the table.
        auto const removed =
            aspace->arch_aspace().unmap(c + 9 * PAGE_SIZE, 1, mm::UnmapOptions::None, nullptr) ==
            Status::Ok;
        auto const again = fault_at(*aspace, c + 9 * PAGE_SIZE, WRITE);
        EXPECT_TRUE(populate && removed && again.present && again.size == PAGE_SIZE,
                    "a fault in a window split before maps its own page only");

        // A clone sees the block read-only as one leaf; a write splits it and
        // copies only the page written.
        ustl::Arc<mm::VmObjectPaged> origin;
        ustl::Arc<mm::VmObjectPaged> clone;
        auto const d = at(512);
        u32 const word = 0x5eed'0003;
        auto shared = Translation{};
        if (make_large(LARGE, &origin) &&
            origin->write(&word, 3 * PAGE_SIZE, sizeof(word)) == Status::Ok &&
            origin->create_clone(0, LARGE, &clone) == Status::Ok &&
            root.map_vmo(d, LARGE, 0, rw, mm::VmaFlags::PermMask, clone, mm::MapOptions::Lazy,
                         "largepage.test clone")
                .is_ok()) {
            shared = fault_at(*aspace, d + 9 * PAGE_SIZE, READ);
            shared.pa -= 9 * PAGE_SIZE;
        }
        EXPECT_TRUE(shared.present && shared.size == LARGE && !writable(shared) &&
                        std::to_underlying(shared.pa) % LARGE == 0,
                    "a read of a clone maps the shared block as one read-only leaf");
        auto written = Translation{};
        if (shared.present) {
            written = fault_at(*aspace, d + 3 * PAGE_SIZE, WRITE);
        }
        auto const neighbour = translate(*aspace, d + 4 * PAGE_SIZE);
        EXPECT_TRUE(written.present && written.size == PAGE_SIZE && writable(written) &&
                        written.pa != shared.pa + 3 * PAGE_SIZE && neighbour.present &&
                        neighbour.size == PAGE_SIZE && !writable(neighbour) &&
                        neighbour.pa == shared.pa + 4 * PAGE_SIZE,
                    "a write to one page splits the leaf: that page is a writable copy, its "
                    "neighbour still the block, read-only");

        // The clone takes the block's first page for itself -- the origin wrote
        // its own copy first, so the clone is the page's last viewer -- while
        // the other 511 stay shared. The window is still physically contiguous,
        // but it is not one node's: a writable leaf over it would let the clone
        // write the pages the origin still sees.
        ustl::Arc<mm::VmObjectPaged> origin2;
        ustl::Arc<mm::VmObjectPaged> clone2;
        auto const e = at(640);
        auto took = Translation{};
        if (make_large(LARGE, &origin2) &&
            origin2->write(&word, 5 * PAGE_SIZE, sizeof(word)) == Status::Ok &&
            origin2->create_clone(0, LARGE, &clone2) == Status::Ok &&
            origin2->write(&word, 0, sizeof(word)) == Status::Ok &&
            root.map_vmo(e, LARGE, 0, rw, mm::VmaFlags::PermMask, clone2, mm::MapOptions::Lazy,
                         "largepage.test migrate")
                .is_ok()) {
            took = fault_at(*aspace, e, WRITE);
        }
        auto const next_page = translate(*aspace, e + PAGE_SIZE);
        EXPECT_TRUE(took.present && took.size == PAGE_SIZE && writable(took) &&
                        (!next_page.present || !writable(next_page)),
                    "a window whose first page the clone took, the rest still shared, is no block");

        // The walk over every mapping of a node's pages, unmapping one page of a
        // leaf as take_pages() will: the leaf splits into the table set aside
        // for it, the page goes, and its neighbours stay, 4 KiB entries to the
        // same frames. The object still holds the page, so a read maps it again.
        ustl::Arc<mm::VmObjectPaged> walked;
        auto const f = at(768);
        auto whole = Translation{};
        if (map_large(root, f, LARGE, 0, mm::MapOptions::Lazy, &walked)) {
            whole = fault_at(*aspace, f, WRITE);
        }
        auto const held_with_leaf = mm::PageTableDeposits::nr_held();
        auto const is_leaf = whole.present && whole.size == LARGE;
        if (is_leaf) {
            unmap_through_node(*walked, 3 * PAGE_SIZE, PAGE_SIZE);
        }
        auto const gone = translate(*aspace, f + 3 * PAGE_SIZE);
        auto const beside = translate(*aspace, f + 4 * PAGE_SIZE);
        EXPECT_TRUE(is_leaf && !gone.present && beside.present && beside.size == PAGE_SIZE &&
                        beside.pa == whole.pa + 4 * PAGE_SIZE &&
                        mm::PageTableDeposits::nr_held() + 1 == held_with_leaf,
                    "an unmap through the node splits a leaf into the table set aside for it, and "
                    "only the page unmapped goes");
        auto const back = fault_at(*aspace, f + 3 * PAGE_SIZE, READ);
        EXPECT_TRUE(is_leaf && back.present && back.size == PAGE_SIZE &&
                        back.pa == whole.pa + 3 * PAGE_SIZE,
                    "the object still holds the page: a read maps it again, small");

        // Placed by the VMM, a large-page object's mapping starts where its
        // object offset falls in a block: at a block boundary for offset 0, and
        // 4 KiB past one for 4 KiB, whose window at object offset 2 MiB is then
        // one whole block.
        auto const anywhere = [&](usize const size, u64 const offset,
                                  ustl::Arc<mm::VmObjectPaged> *const out) {
            ustl::Arc<mm::VmMapping> placed;
            if (!make_large(size + offset, out)) {
                return placed;
            }
            auto mapped = root.map_vmo(VirtAddr::Anywhere, size, offset, rw, mm::VmaFlags::PermMask,
                                       *out, mm::MapOptions::Lazy, "largepage.test anywhere");
            if (mapped.is_ok()) {
                placed = mapped.unwrap();
            }
            return placed;
        };
        // A page placed by the VMM first, so that the lowest free address is
        // 4 KiB past a block boundary: a mapping that lands on one does so
        // because it was placed there, not by luck.
        ustl::Arc<mm::VmObjectPaged> spacer_vmo;
        auto const spacer =
            mm::VmObjectPaged::create(mm::ALLOC_USER, PAGE_SIZE, mm::VmoFlags::Lazy, &spacer_vmo) ==
                Status::Ok &&
            root.map_vmo(VirtAddr::Anywhere, PAGE_SIZE, 0, rw, mm::VmaFlags::PermMask, spacer_vmo,
                         mm::MapOptions::Lazy, "largepage.test spacer")
                .is_ok();
        ustl::Arc<mm::VmObjectPaged> at_zero;
        auto const zero = anywhere(2 * LARGE, 0, &at_zero);
        auto zero_leaf = Translation{};
        if (zero) {
            zero_leaf = fault_at(*aspace, zero->base() + 5 * PAGE_SIZE, WRITE);
        }
        EXPECT_TRUE(spacer && zero && std::to_underlying(zero->base()) % LARGE == 0 &&
                        zero_leaf.present && zero_leaf.size == LARGE,
                    "placed by the VMM at offset 0, the mapping starts on a block boundary, and a "
                    "fault maps a leaf");
        ustl::Arc<mm::VmObjectPaged> at_page;
        auto const page = anywhere(2 * LARGE, PAGE_SIZE, &at_page);
        auto page_leaf = Translation{};
        if (page) {
            page_leaf =
                fault_at(*aspace, page->base() + (LARGE - PAGE_SIZE) + 5 * PAGE_SIZE, WRITE);
        }
        EXPECT_TRUE(page && std::to_underlying(page->base()) % LARGE == PAGE_SIZE &&
                        page_leaf.present && page_leaf.size == LARGE,
                    "placed by the VMM at offset 4 KiB, it starts 4 KiB past a boundary, and its "
                    "window at object offset 2 MiB maps a leaf");

        attach(previous);
        EXPECT_OK(aspace->destroy(), "the address space of the mapping checks is destroyed");
    }

    KTEST(LargePage, LeavesSplitWithSetAsideTables) {
        auto const held_before = mm::PageTableDeposits::nr_held();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;

        auto *const block =
            mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::Zero, ORDER).value_or(nullptr);
        ASSERT_NONNULL(block, "allocate a 2 MiB block to map");
        auto const pa = mm::frame_to_phys(block);

        {
            ustl::Arc<mm::VmAspace> aspace;
            if (auto const status = mm::VmAspace::create(
                    USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                    mm::VmasFlags::None, "largepage.test", &aspace);
                status != Status::Ok) {
                log::error("largepage: could not create an address space: {}", to_string(status));
                mm::free_frames(block, ORDER);
                FAIL("create the address space");
            }
            auto &arch = aspace->arch_aspace();
            auto const va = ustl::round_up(USER_ASPACE_BASE + 64_mb, LARGE);

            // One leaf, with one table set aside for it.
            auto const mapped =
                arch.map(va, pa, LARGE / PAGE_SIZE, rw,
                         mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure,
                         nullptr) == Status::Ok;
            auto const leaf = translate(*aspace, va + 7 * PAGE_SIZE);
            EXPECT_TRUE(mapped && leaf.present && leaf.size == LARGE &&
                            leaf.pa == pa + 7 * PAGE_SIZE &&
                            mm::PageTableDeposits::nr_held() == held_before + 1,
                        "a contiguous 2 MiB block is mapped as one leaf, with a table set aside");

            // An unmap of one page splits it into that table.
            auto const unmapped =
                arch.unmap(va + 7 * PAGE_SIZE, 1, mm::UnmapOptions::None, nullptr) == Status::Ok;
            EXPECT_TRUE(unmapped && !translate(*aspace, va + 7 * PAGE_SIZE).present &&
                            is_small_to(*aspace, va, pa) &&
                            is_small_to(*aspace, va + 8 * PAGE_SIZE, pa + 8 * PAGE_SIZE) &&
                            is_small_to(*aspace, va + LARGE - PAGE_SIZE, pa + LARGE - PAGE_SIZE) &&
                            mm::PageTableDeposits::nr_held() == held_before,
                        "an unmap of one page splits the leaf into its table: the page is gone, "
                        "the rest are small entries to the same frames");

            // A protect of another page narrows that page only.
            auto const protected_one = arch.protect(va, 1, ro) == Status::Ok;
            auto const first = translate(*aspace, va);
            auto const second = translate(*aspace, va + PAGE_SIZE);
            EXPECT_TRUE(protected_one && first.present && !(first.flags & mm::MmuFlags::Writable) &&
                            second.present && !!(second.flags & mm::MmuFlags::Writable),
                        "a protect of the first page takes Write from it alone");

            EXPECT_TRUE(arch.unmap(va, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) ==
                                Status::Ok &&
                            !translate(*aspace, va).present,
                        "the rest of the range is unmapped");

            // The split range stays small: its second-level entry now points at
            // a table, and unmapping never frees one, so mapping the block there
            // again fills that table with small entries and sets nothing aside.
            auto const remapped =
                arch.map(va, pa, LARGE / PAGE_SIZE, rw,
                         mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure,
                         nullptr) == Status::Ok;
            EXPECT_TRUE(remapped && is_small_to(*aspace, va + 7 * PAGE_SIZE, pa + 7 * PAGE_SIZE) &&
                            mm::PageTableDeposits::nr_held() == held_before &&
                            arch.unmap(va, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) ==
                                Status::Ok,
                        "the split range mapped again stays small entries");

            // A leaf mapped in the next 2 MiB, never split, and unmapped whole
            // gives its table back.
            auto const next = va + LARGE;
            auto const again =
                arch.map(next, pa, LARGE / PAGE_SIZE, rw,
                         mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure,
                         nullptr) == Status::Ok;
            auto const held_with_leaf = mm::PageTableDeposits::nr_held();
            auto const again_leaf = translate(*aspace, next);
            auto const whole =
                arch.unmap(next, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) == Status::Ok;
            EXPECT_TRUE(again && again_leaf.present && again_leaf.size == LARGE &&
                            held_with_leaf == held_before + 1 && whole &&
                            !translate(*aspace, next).present &&
                            mm::PageTableDeposits::nr_held() == held_before,
                        "a leaf unmapped whole gives its table back");

            // A leaf still mapped when the address space goes: no unmap gives
            // its table back, so the teardown of the page tables must. It is
            // mapped straight through the page table, as the ones above, so
            // VmAspace::destroy(), which unmaps what its mappings hold, leaves
            // it for ~VmAspace to find, and to report in a warning.
            auto const left =
                arch.map(next, pa, LARGE / PAGE_SIZE, rw,
                         mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure,
                         nullptr) == Status::Ok;
            auto const left_leaf = translate(*aspace, next);
            EXPECT_TRUE(left && left_leaf.present && left_leaf.size == LARGE &&
                            mm::PageTableDeposits::nr_held() == held_before + 1,
                        "a leaf is left mapped for the teardown, with its table set aside");

            EXPECT_OK(aspace->destroy(), "the address space is destroyed");
        }
        EXPECT_EQ(mm::PageTableDeposits::nr_held(), held_before,
                  "with the address space gone, every table set aside is back, the one of the "
                  "leaf left mapped too");
        mm::free_frames(block, ORDER);

        check_blocks();
        check_mappings();
    }
} // namespace
} // namespace ours
