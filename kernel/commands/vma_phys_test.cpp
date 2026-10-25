/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmaphys.test`: VmMapping::map() of a physical object whose range has more
// than one protection maps each page to its own frame.
//
// map_physical() walks the range one protection region at a time, and each
// region's pages are counted from that region's own start. The physical
// address has to be counted from the same place, or every region after the
// first maps the frames of the start of the range again.
//
// Why a console command: no program can reach it. There is no vma_protect
// system call, and map() of a range already mapped is called only from the
// kernel.
//
// It builds an address space of its own, maps two contiguous frames as a
// physical object without Populate (nothing in the page tables yet), makes
// the second page read-only -- two protection regions -- and calls map() over
// both pages, then reads each page-table entry back.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr u64 P = PAGE_SIZE;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("vmaphys.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmaphys.test: FAIL  {}", name);
    }

    /// What the page table holds for |va|: its frame and permissions, or
    /// nothing.
    struct Entry {
        bool present = false;
        PhysAddr pa = 0;
        mm::MmuFlags flags{};
    };

    auto entry_at(mm::VmAspace &aspace, VirtAddr const va) -> Entry {
        Entry entry;
        entry.present = aspace.arch_aspace().query(va, &entry.pa, &entry.flags) == Status::Ok;
        return entry;
    }

    auto check_two_regions(Tally &tally, mm::VmAspace &aspace) -> void {
        PhysAddr phys = 0;
        auto *const frames = mm::alloc_frames(mm::GAF_KERNEL, &phys, 1);
        if (!frames) {
            verdict(tally, false, "two contiguous frames are allocated");
            return;
        }

        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_phys(phys, mm::VmPlacement::anywhere(), 2 * P, rw,
                                      mm::VmMapOption::None, "vmaphys.test");
        if (mapping.is_err()) {
            verdict(tally, false, "the two frames are mapped as a physical object");
            mm::free_frames(frames, 1);
            return;
        }
        auto const base = mapping.unwrap()->base();
        verdict(tally, !entry_at(aspace, base).present && !entry_at(aspace, base + P).present,
                "a physical object mapped without Populate has nothing in the page tables");

        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;
        verdict(tally, root->protect(base + P, P, ro) == Status::Ok,
                "its second page is made read-only: two protection regions");

        verdict(tally, mapping.unwrap()->map(base, 2 * P, mm::MapOptions::None) == Status::Ok,
                "map() over both regions is Ok");
        auto const first = entry_at(aspace, base);
        auto const second = entry_at(aspace, base + P);
        log::info("vmaphys.test:   frames at {:#x}; page 0 -> {:#x}, page 1 -> {:#x}", phys,
                  first.pa, second.pa);
        verdict(tally,
                first.present && first.pa == phys && !!(first.flags & mm::MmuFlags::Writable),
                "the first region's page is on the first frame, writable");
        verdict(tally,
                second.present && second.pa == phys + P && !(second.flags & mm::MmuFlags::Writable),
                "the second region's page is on the second frame, read-only");

        auto _ = root->unmap(base, 2 * P);
        mm::free_frames(frames, 1);
    }

    auto vmaphys_test_command(int, char **) -> int {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmaphys.test", &aspace);
            status != Status::Ok) {
            log::error("vmaphys.test: could not create an address space: {}", to_string(status));
            return 1;
        }

        Tally tally;
        check_two_regions(tally, *aspace);
        verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");

        if (tally.failed != 0) {
            log::error("vmaphys.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmaphys.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmaPhysTest, "vmaphys.test", ours::vmaphys_test_command,
             "check that map() of a physical mapping with two protections maps each page to its "
             "own frame");
