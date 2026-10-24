/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `largepage.test`: 2 MiB leaves in a user address space, and the tables set
// aside to split them (.agents/tasks/large-pages).
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
//   - when the address space goes, every table set aside has come back.
//
// Why a console command: the deposits live in the descriptors of real page
// tables, built by the real allocator under the page table's lock, which the
// host test of the paging engine stands in for with a pool.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_table_deposits.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize LARGE = 2_mb;
    constexpr usize ORDER = 9;
    static_assert((usize{1} << ORDER) * PAGE_SIZE == LARGE);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("largepage.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("largepage.test: FAIL  {}", name);
    }

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

    auto largepage_test_command(int, char **) -> int {
        Tally tally;
        auto const held_before = mm::PageTableDeposits::nr_held();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;

        auto *const block = mm::alloc_frames(mm::GAF_KERNEL | mm::Gaf::Zero, ORDER);
        if (block == nullptr) {
            log::error("largepage.test: no 2 MiB block to map");
            return 1;
        }
        auto const pa = mm::frame_to_phys(block);

        {
            ustl::Arc<mm::VmAspace> aspace;
            if (auto const status = mm::VmAspace::create(
                    USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                    mm::VmasFlags::None, "largepage.test", &aspace);
                status != Status::Ok) {
                log::error("largepage.test: could not create an address space: {}",
                           to_string(status));
                mm::free_frames(block, ORDER);
                return 1;
            }
            auto &arch = aspace->arch_aspace();
            auto const va = ustl::mm::align_up(USER_ASPACE_BASE + 64_mb, LARGE);

            // One leaf, with one table set aside for it.
            auto const mapped = arch.map(va, pa, LARGE / PAGE_SIZE, rw,
                                         mm::MapOptions::ErrorIfExisting, nullptr) == Status::Ok;
            auto const leaf = translate(*aspace, va + 7 * PAGE_SIZE);
            verdict(tally,
                    mapped && leaf.present && leaf.size == LARGE && leaf.pa == pa + 7 * PAGE_SIZE &&
                        mm::PageTableDeposits::nr_held() == held_before + 1,
                    "a contiguous 2 MiB block is mapped as one leaf, with a table set aside");

            // An unmap of one page splits it into that table.
            auto const unmapped =
                arch.unmap(va + 7 * PAGE_SIZE, 1, mm::UnmapOptions::None, nullptr) == Status::Ok;
            verdict(tally,
                    unmapped && !translate(*aspace, va + 7 * PAGE_SIZE).present &&
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
            verdict(tally,
                    protected_one && first.present && !(first.flags & mm::MmuFlags::Writable) &&
                        second.present && !!(second.flags & mm::MmuFlags::Writable),
                    "a protect of the first page takes Write from it alone");

            verdict(tally,
                    arch.unmap(va, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) ==
                            Status::Ok &&
                        !translate(*aspace, va).present,
                    "the rest of the range is unmapped");

            // The split range stays small: its second-level entry now points at
            // a table, and unmapping never frees one, so mapping the block there
            // again fills that table with small entries and sets nothing aside.
            auto const remapped = arch.map(va, pa, LARGE / PAGE_SIZE, rw,
                                           mm::MapOptions::ErrorIfExisting, nullptr) == Status::Ok;
            verdict(tally,
                    remapped && is_small_to(*aspace, va + 7 * PAGE_SIZE, pa + 7 * PAGE_SIZE) &&
                        mm::PageTableDeposits::nr_held() == held_before &&
                        arch.unmap(va, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) ==
                            Status::Ok,
                    "the split range mapped again stays small entries");

            // A leaf mapped in the next 2 MiB, never split, and unmapped whole
            // gives its table back.
            auto const next = va + LARGE;
            auto const again = arch.map(next, pa, LARGE / PAGE_SIZE, rw,
                                        mm::MapOptions::ErrorIfExisting, nullptr) == Status::Ok;
            auto const held_with_leaf = mm::PageTableDeposits::nr_held();
            auto const again_leaf = translate(*aspace, next);
            auto const whole =
                arch.unmap(next, LARGE / PAGE_SIZE, mm::UnmapOptions::None, nullptr) == Status::Ok;
            verdict(tally,
                    again && again_leaf.present && again_leaf.size == LARGE &&
                        held_with_leaf == held_before + 1 && whole &&
                        !translate(*aspace, next).present &&
                        mm::PageTableDeposits::nr_held() == held_before,
                    "a leaf unmapped whole gives its table back");

            verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");
        }
        verdict(tally, mm::PageTableDeposits::nr_held() == held_before,
                "with the address space gone, every table set aside is back");
        mm::free_frames(block, ORDER);

        if (tally.failed != 0) {
            log::error("largepage.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("largepage.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(LargePageTest, "largepage.test", ours::largepage_test_command,
             "check 2 MiB leaves in a user address space, split with a table set aside");
