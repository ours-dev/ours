/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaPhys.MapsEachPageToItsOwnFrame: VmMapping::map() of a physical object
// whose range has more than one protection maps each page to its own frame.
//
// map_physical() walks the range one protection region at a time, and each
// region's pages are counted from that region's own start. The physical
// address has to be counted from the same place, or every region after the
// first maps the frames of the start of the range again.
//
// Why an in-kernel test: no program can reach it. There is no vma_protect
// system call, and map() of a range already mapped is called only from the
// kernel.
//
// It builds an address space of its own, maps two contiguous frames as a
// physical object without Populate (nothing in the page tables yet), makes
// the second page read-only -- two protection regions -- and calls map() over
// both pages, then reads each page-table entry back.
//
// It also checks that the page table's OverwriteIfExisting reaches the CPU:
// a page read through its entry, so that this CPU caches the translation,
// then written over with another frame, reads the new frame next time.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr u64 P = PAGE_SIZE;

    /// What the page table holds for |va|: its frame and permissions, or
    /// nothing.
    struct Entry {
        bool present = false;
        PhysAddr pa{};
        mm::MmuFlags flags{};
    };

    auto entry_at(mm::VmAspace &aspace, VirtAddr const va) -> Entry {
        Entry entry;
        entry.present = aspace.arch_aspace().query(va, &entry.pa, &entry.flags) == Status::Ok;
        return entry;
    }

    auto check_two_regions(mm::VmAspace &aspace) -> void {
        PhysAddr phys{};
        auto *const frames = mm::alloc_frames(mm::ALLOC_KERNEL, &phys, 1).value_or(nullptr);
        if (!frames) {
            EXPECT_TRUE(false, "two contiguous frames are allocated");
            return;
        }

        auto root = aspace.root_vma();
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto mapping = root->map_phys(phys, VirtAddr::Anywhere, 2 * P, rw, mm::MapOptions::Lazy,
                                      "vmaphys.test");
        if (mapping.is_err()) {
            EXPECT_TRUE(false, "the two frames are mapped as a physical object");
            mm::free_frames(frames, 1);
            return;
        }
        auto const base = mapping.unwrap()->base();
        EXPECT_TRUE(!entry_at(aspace, base).present && !entry_at(aspace, base + P).present,
                    "a physical object mapped without Populate has nothing in the page tables");
        EXPECT_TRUE(aspace.soft_fault(base, mm::VmFault::Cause::Absent) == Status::NotSupported &&
                        !entry_at(aspace, base).present,
                    "a physical mapping cannot install a frame on demand");

        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;
        EXPECT_TRUE(root->protect(base + P, P, ro) == Status::Ok,
                    "its second page is made read-only: two protection regions");

        EXPECT_TRUE(mapping.unwrap()->map(base, 2 * P, mm::MapOptions::Resident) == Status::Ok,
                    "map() over both regions is Ok");
        auto const first = entry_at(aspace, base);
        auto const second = entry_at(aspace, base + P);
        log::info("vmaphys.test:   frames at {:#x}; page 0 -> {:#x}, page 1 -> {:#x}", phys,
                  first.pa, second.pa);
        EXPECT_TRUE(first.present && first.pa == phys && !!(first.flags & mm::MmuFlags::Writable),
                    "the first region's page is on the first frame, writable");
        EXPECT_TRUE(second.present && second.pa == phys + P &&
                        !(second.flags & mm::MmuFlags::Writable),
                    "the second region's page is on the second frame, read-only");
        EXPECT_TRUE(root->protect(base + P, P, mm::MmuFlags{}) == Status::InvArgs &&
                        entry_at(aspace, base + P).flags == second.flags,
                    "protect rejects empty permissions and leaves the physical PTE unchanged");

        auto _ = root->unmap(base, 2 * P);
        mm::free_frames(frames, 1);
    }

    /// Puts the calling thread on |next| and loads its page tables, and hands
    /// back what it was on before. Preemption is off across the two steps, as
    /// in usercopy.test's attach(): the scheduler loads the page tables of
    /// thread->aspace() on every switch, and the two must agree.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    /// A page is read through its entry, so that this CPU caches the
    /// translation; written over with another frame (OverwriteIfExisting);
    /// and read again. Without the invalidation the overwrite owes, the
    /// second read would still reach the first frame. Preemption is off from
    /// the first read to the second, so that both are made on the CPU that
    /// cached it. The reads are the capturing copies, which may run so.
    auto check_overwrite(mm::VmAspace &aspace) -> void {
        constexpr u64 FIRST = 0x0f1e'2d3c'4b5a'6978;
        constexpr u64 SECOND = 0x8796'a5b4'c3d2'e1f0;
        constexpr auto RW = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;

        PhysAddr first_pa{};
        PhysAddr second_pa{};
        auto *const first = mm::alloc_frames(mm::ALLOC_KERNEL, &first_pa, 0).value_or(nullptr);
        auto *const second = mm::alloc_frames(mm::ALLOC_KERNEL, &second_pa, 0).value_or(nullptr);
        if (!first || !second) {
            EXPECT_TRUE(false, "two frames are allocated for the overwrite");
            if (first) {
                mm::free_frames(first, 0);
            }
            if (second) {
                mm::free_frames(second, 0);
            }
            return;
        }
        *reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt(first_pa)) = FIRST;
        *reinterpret_cast<u64 *>(mm::PhysMap::phys_to_virt(second_pa)) = SECOND;

        auto root = aspace.root_vma();
        auto mapping = root->map_phys(first_pa, VirtAddr::Anywhere, P, RW, mm::MapOptions::Resident,
                                      "vmaphys.test overwrite");
        if (mapping.is_err()) {
            EXPECT_TRUE(false, "the first frame is mapped, populated");
            mm::free_frames(first, 0);
            mm::free_frames(second, 0);
            return;
        }
        auto const va = mapping.unwrap()->base();
        auto &arch_aspace = aspace.arch_aspace();

        auto *const previous = attach(&aspace);
        u64 before = 0;
        u64 after = 0;
        auto read_first = Status::InternalError;
        auto overwritten = Status::InternalError;
        auto read_second = Status::InternalError;
        {
            task::AutoPreemptDisabler stay_on_this_cpu;
            read_first =
                user_copy::copy_from_user_capture_faults(&before, va, sizeof(before)).status;
            overwritten = arch_aspace.map(va, second_pa, 1, RW,
                                          mm::ArchMapOptions::OverwriteIfExisting |
                                              mm::ArchMapOptions::UndoOnFailure,
                                          nullptr);
            read_second =
                user_copy::copy_from_user_capture_faults(&after, va, sizeof(after)).status;
        }
        // After the reads: a failed map with Undo invalidates as well, and
        // would hide a missing invalidation if it came between them.
        auto const refused = arch_aspace.map(
            va, first_pa, 1, RW,
            mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure, nullptr);
        auto const still = entry_at(aspace, va);
        attach(previous);

        log::info("vmaphys.test:   overwrite: read {:#x}, then {:#x}", before, after);
        EXPECT_TRUE(read_first == Status::Ok && overwritten == Status::Ok &&
                        read_second == Status::Ok && before == FIRST && after == SECOND,
                    "a page written over with another frame reads the new frame on the CPU that "
                    "had cached the old one");
        EXPECT_TRUE(refused == Status::AlreadyExists && still.present && still.pa == second_pa,
                    "ErrorIfExisting over the page is AlreadyExists, and leaves it as it was");

        auto _ = root->unmap(va, P);
        mm::free_frames(first, 0);
        mm::free_frames(second, 0);
    }

    KTEST(VmaPhys, MapsEachPageToItsOwnFrame) {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmaphys.test", &aspace);
            status != Status::Ok) {
            FAIL("vmaphys.test: could not create an address space: {}", to_string(status));
        }

        check_two_regions(*aspace);
        check_overwrite(*aspace);
        EXPECT_TRUE(aspace->destroy() == Status::Ok, "the address space is destroyed");
    }
} // namespace
} // namespace ours
