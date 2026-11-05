/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmaseal.test`: a sealed mapping (MapOptions::Sealed) refuses to be
// unmapped or have its protection changed, and only that one.
//
// Why a console command: the refusals live in mm::VmArea::unmap() and
// protect(), and no program can reach protect() yet -- there is no
// vma_protect system call -- so the shell cannot test that one from ring 3.
// The unmap refusals it can reach through vma_unmap, and system/app/shell
// checks them there too (check_vma_unmap()), along with what only a program
// can show: that a launcher's request for a sealed mapping is accepted and
// that the vDSO is refused without it.
//
// It builds an address space of its own and maps, at fixed addresses so that
// what neighbours what is known:
//
//     SEALED   two pages, sealed
//     PLAIN    one page, not sealed, right after SEALED
//     AREA     a sub-area of four pages, with a sealed page inside it
//
// and checks that:
//
//   - unmapping SEALED whole, or trimming its first page, is AccessDenied;
//   - changing its protection is AccessDenied;
//   - an unmap that covers PLAIN and reaches SEALED is refused whole:
//     PLAIN is still mapped afterwards, the refusal is asked first and
//     nothing is torn down half way;
//   - unmapping AREA, which only a sealed mapping *inside* it makes
//     immutable, is refused too;
//   - PLAIN alone can be protected and unmapped: the refusal is specific to
//     what is sealed, not to the address space;
//   - and the address space with all of that in it can still be destroyed,
//     since taking a space away is not a program changing its shape.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("vmaseal.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmaseal.test: FAIL  {}", name);
    }

    auto make_vmo(usize const size) -> ustl::Arc<mm::VmObject> {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER, size, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            return {};
        }
        return vmo;
    }

    auto check_protect_preflight(Tally &tally, mm::VmAspace &aspace) -> void {
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto RW = READ | mm::MmuFlags::Writable;
        constexpr auto RX = READ | mm::MmuFlags::Executable;
        constexpr auto BASE = USER_ASPACE_BASE + 48_mb;
        auto root = aspace.root_vma();
        auto first =
            root->map_with_vmo(BASE, 3 * PAGE_SIZE, 0, RW, mm::VmaFlags::PermMask,
                               make_vmo(3 * PAGE_SIZE), mm::MapOptions::Commit, "writable");
        auto neighbor =
            root->map_with_vmo(BASE + 3 * PAGE_SIZE, PAGE_SIZE, 0, READ, mm::VmaFlags::Read,
                               make_vmo(PAGE_SIZE), mm::MapOptions::Lazy, "read ceiling");
        verdict(tally, first.is_ok() && neighbor.is_ok(),
                "adjacent mappings have different permission ceilings");
        if (!first || !neighbor) {
            return;
        }
        auto const middle = BASE + PAGE_SIZE;
        PhysAddr pa{};
        mm::MmuFlags entry_flags{};
        verdict(tally,
                root->protect(BASE, 4 * PAGE_SIZE, RX) == Status::InvArgs &&
                    aspace.arch_aspace().query(middle, &pa, &entry_flags) == Status::Ok &&
                    !!(entry_flags & mm::MmuFlags::Writable),
                "a neighbor's ceiling refuses the whole protect before write is removed");
        verdict(tally,
                root->protect(middle, PAGE_SIZE, READ) == Status::Ok &&
                    root->protect(middle, PAGE_SIZE, RW) == Status::Ok,
                "restoring the middle page returns it to the neighboring protection");
        auto &mapping = **first;
        Guard guard{mapping.get_lock()};
        usize regions = 0;
        mapping.for_each_protection_range_locked([&](VirtAddr, usize, mm::MmuFlags) { ++regions; });
        verdict(tally, regions == 1, "equal protection spans coalesce after restoration");
    }

    auto check_registration_undo(Tally &tally, mm::VmAspace &aspace) -> void {
        auto root = aspace.root_vma();
        constexpr auto RX = mm::MmuFlags::Readable | mm::MmuFlags::Executable | mm::MmuFlags::User;
        auto code = root->map_with_vmo(
            VirtAddr::Anywhere, PAGE_SIZE, 0, RX, mm::VmaFlags::PermMask, make_vmo(PAGE_SIZE),
            mm::MapOptions::Sealed | mm::MapOptions::Commit, "test code");
        auto const claimed = code.is_ok() && aspace.claim_vdso();
        verdict(tally, claimed, "the address space accepts one sealed code registration");
        if (!claimed) {
            return;
        }
        auto const address = (*code)->base();
        aspace.set_vdso(*code);
        verdict(tally,
                aspace.vdso().as_ptr() == code->as_ptr() && aspace.vdso_contains(address, 1) &&
                    !aspace.claim_vdso(),
                "the registration retains its mapping and disallows a second claim");
        verdict(tally,
                root->destroy_child(**code) == Status::Ok && !aspace.vdso() &&
                    !aspace.vdso_contains(address, 1),
                "undoing the mapping withdraws the address space's registration");
        auto const reclaimed = aspace.claim_vdso();
        verdict(tally, reclaimed, "withdrawal makes the claim reusable");
        if (reclaimed) {
            aspace.release_vdso();
        }
    }

#if MM_TEST_ALLOC_FAILURE
    auto check_protect_allocation_failure(Tally &tally, mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 80_mb;
        constexpr usize PAGES = 32;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto RW = READ | mm::MmuFlags::Writable;
        auto root = aspace.root_vma();
        auto mapped = root->map_with_vmo(BASE, PAGES * PAGE_SIZE, 0, RW, mm::VmaFlags::PermMask,
                                         make_vmo(PAGES * PAGE_SIZE), mm::MapOptions::Commit,
                                         "protection allocation");
        verdict(tally, mapped.is_ok(), "a writable mapping for protection tree growth");
        if (!mapped) {
            return;
        }
        usize failures = 0;
        for (usize index = 1; index + 1 < PAGES; index += 2) {
            auto const at = BASE + index * PAGE_SIZE;
            Status status;
            bool triggered = false;
            {
                AllocationFailure failure(AllocationSite::Collection);
                status = root->protect(at, PAGE_SIZE, READ);
                triggered = failure.triggered();
            }
            if (!triggered && status == Status::Ok) {
                continue;
            }
            ++failures;
            PhysAddr pa{};
            mm::MmuFlags flags{};
            verdict(tally,
                    triggered && status == Status::OutOfMem &&
                        aspace.arch_aspace().query(at, &pa, &flags) == Status::Ok &&
                        !!(flags & mm::MmuFlags::Writable) &&
                        root->protect(at, PAGE_SIZE, READ) == Status::Ok,
                    "an allocation failure preserves the PTE until protect is retried");
        }
        verdict(tally, failures != 0, "the protection tree needed allocation while splitting");
    }

    auto check_unmap_allocation_failure(Tally &tally, mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 112_mb;
        constexpr usize PAGES = 32;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto root = aspace.root_vma();
        auto mapped = root->map_with_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask,
                                         make_vmo(PAGES * PAGE_SIZE), mm::MapOptions::Commit,
                                         "unmap allocation");
        verdict(tally, mapped.is_ok(), "a committed mapping for partial unmap");
        if (!mapped) {
            return;
        }
        usize failures = 0;
        for (usize index = 1; index + 1 < PAGES; index += 2) {
            auto const at = BASE + index * PAGE_SIZE;
            Status status;
            bool triggered;
            {
                AllocationFailure failure(AllocationSite::Collection);
                status = root->unmap(at, PAGE_SIZE);
                triggered = failure.triggered();
            }
            PhysAddr pa{};
            mm::MmuFlags flags{};
            if (triggered) {
                ++failures;
                verdict(tally,
                        status == Status::OutOfMem &&
                            aspace.arch_aspace().query(at, &pa, &flags) == Status::Ok,
                        "a failed partial unmap preserves the original PTE");
                status = root->unmap(at, PAGE_SIZE);
            }
            verdict(tally,
                    status == Status::Ok &&
                        aspace.arch_aspace().query(at, &pa, &flags) == Status::NotFound &&
                        aspace.soft_fault(at, mm::VmfCause::Absent) == Status::AccessDenied &&
                        aspace.arch_aspace().query(at - PAGE_SIZE, &pa, &flags) == Status::Ok,
                    "partial unmap removes the target translation and prevents a refault");
        }
        verdict(tally, failures != 0, "the unmap needed allocation while splitting");
    }

    auto check_aspace_allocation_failures(Tally &tally) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        auto const fails = [](AllocationSite const site, usize const skip, Status const expected) {
            ustl::Arc<mm::VmAspace> aspace;
            AllocationFailure failure(site, skip);
            auto const status = mm::VmAspace::create(mm::VmasKind::User, mm::VmasFlags::None,
                                                     "failed address space", &aspace);
            return status == expected && failure.triggered() && !aspace;
        };
        verdict(tally, fails(AllocationSite::Object, 0, Status::OutOfMem),
                "address-space object allocation failure leaves no reference");
        verdict(tally, fails(AllocationSite::PageTable, 0, Status::NoMemory),
                "a missing root table leaves the address space unpublished");
        verdict(tally, fails(AllocationSite::Object, 1, Status::OutOfMem),
                "a missing root VMA unwinds an initialized page table");
    }

    auto check_allocation_failures(Tally &tally, mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 64_mb;
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto root = aspace.root_vma();
        ustl::Arc<mm::VmArea> parent;
        auto vmo = make_vmo(PAGE_SIZE);
        auto const created =
            root->create_subvma(BASE, 2 * PAGE_SIZE, PERMS | mm::VmaFlags::CanMapSpecific,
                                "allocation failures", &parent);
        verdict(tally, created == Status::Ok && vmo, "a fresh area for failed insertions");
        if (!parent || !vmo) {
            return;
        }

        ustl::Arc<mm::VmArea> child;
        bool failed_insert = false;
        {
            AllocationFailure failure(AllocationSite::Collection);
            auto const status =
                parent->create_subvma(BASE, PAGE_SIZE, PERMS, "failed child", &child);
            failed_insert = status == Status::OutOfMem && failure.triggered() && !child;
        }
        verdict(tally, failed_insert, "an allocation failure leaves the sub-area unpublished");

        bool failed_reservation = false;
        {
            AllocationFailure failure(AllocationSite::Heap);
            auto const status = parent->reserve(BASE, PAGE_SIZE, READ, "failed reservation");
            failed_reservation = status == Status::OutOfMem && failure.triggered();
        }
        verdict(tally, failed_reservation, "a failed reservation leaves the range available");
        bool failed_anon = false;
        {
            AllocationFailure failure(AllocationSite::Heap);
            auto mapped = parent->map_anon(BASE, PAGE_SIZE, READ, mm::MapOptions::Resident,
                                           "failed anonymous VMO");
            failed_anon =
                mapped.is_err() && mapped.unwrap_err() == Status::OutOfMem && failure.triggered();
        }
        verdict(tally, failed_anon, "an anonymous mapping propagates hierarchy allocation failure");

        struct Case {
            AllocationSite site;
            usize skip;
            Status error;
            char const *description;
        };
        constexpr Case CASES[] = {
            {AllocationSite::Object, 0, Status::OutOfMem, "mapping object allocation fails"},
            {AllocationSite::Collection, 0, Status::OutOfMem, "protection range allocation fails"},
            {AllocationSite::Collection, 1, Status::OutOfMem, "mapping tree insertion fails"},
            {AllocationSite::Frame, 0, Status::OutOfMem, "backing page allocation fails"},
            {AllocationSite::PageTable, 0, Status::NoMemory, "page-table allocation fails"},
        };
        for (auto const &entry: CASES) {
            bool failed = false;
            {
                AllocationFailure failure(entry.site, entry.skip);
                auto mapped = parent->map_with_vmo(BASE, PAGE_SIZE, 0, READ, PERMS, vmo,
                                                   mm::MapOptions::Commit, entry.description);
                failed =
                    mapped.is_err() && mapped.unwrap_err() == entry.error && failure.triggered();
            }
            ustl::Arc<mm::VmMapping> leftover;
            PhysAddr pa{};
            mm::MmuFlags flags{};
            verdict(tally,
                    failed && aspace.find_mapping(BASE, &leftover) == Status::NotFound &&
                        aspace.arch_aspace().query(BASE, &pa, &flags) == Status::NotFound,
                    entry.description);
        }
        auto retried = parent->map_with_vmo(BASE, PAGE_SIZE, 0, READ, PERMS, vmo,
                                            mm::MapOptions::Commit, "mapping retry");
        verdict(tally, retried.is_ok(), "the same address can be mapped after every failed step");
    }

    auto check_partial_map_failure(Tally &tally, mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 96_mb;
        constexpr usize PAGES = 34;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto root = aspace.root_vma();
        auto vmo = make_vmo(PAGES * PAGE_SIZE);
        if (!vmo) {
            verdict(tally, false, "a VMO for partial map rollback is made");
            return;
        }
        bool failed;
        {
            // The first batch is installed and the next one has begun when the last page fails.
            AllocationFailure failure(AllocationSite::Frame, PAGES - 1);
            auto mapped =
                root->map_with_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask, vmo,
                                   mm::MapOptions::Commit, "failed partial map");
            failed =
                mapped.is_err() && mapped.unwrap_err() == Status::OutOfMem && failure.triggered();
        }
        ustl::Arc<mm::VmMapping> leftover;
        bool unmapped = true;
        for (usize index = 0; index < PAGES; ++index) {
            PhysAddr pa{};
            mm::MmuFlags flags{};
            unmapped &= aspace.arch_aspace().query(BASE + index * PAGE_SIZE, &pa, &flags) ==
                        Status::NotFound;
        }
        verdict(tally,
                failed && unmapped && aspace.find_mapping(BASE, &leftover) == Status::NotFound,
                "an allocation failure withdraws the mapping and every installed PTE");
        auto retried = root->map_with_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask,
                                          vmo, mm::MapOptions::Commit, "partial map retry");
        verdict(tally, retried.is_ok(), "the same window can be remapped after partial failure");
    }
#endif

    auto vmaseal_test_command(int, char **) -> int {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmaseal.test", &aspace);
            status != Status::Ok) {
            log::error("vmaseal.test: could not create an address space: {}",
                       static_cast<int>(status));
            return 1;
        }

        // Not const: ustl::Arc passes its own constness through to the area.
        auto root = aspace->root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto PERMS = mm::VmaFlags::PermMask;

        constexpr auto SEALED = USER_ASPACE_BASE + 16_mb;
        constexpr auto PLAIN = SEALED + 2 * PAGE_SIZE;
        constexpr auto AREA = USER_ASPACE_BASE + 32_mb;

        Tally tally;

        auto sealed =
            root->map_with_vmo(SEALED, 2 * PAGE_SIZE, 0, flags, PERMS, make_vmo(2 * PAGE_SIZE),
                               mm::MapOptions::Sealed, "vmaseal.test sealed");
        auto plain = root->map_with_vmo(PLAIN, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                                        mm::MapOptions::Lazy, "vmaseal.test plain");
        ustl::Arc<mm::VmArea> area;
        auto const area_status = root->create_subvma(
            AREA, 4 * PAGE_SIZE, PERMS | mm::VmaFlags::CanMapSpecific, "vmaseal.test area", &area);
        verdict(tally, sealed.is_ok() && plain.is_ok() && area_status == Status::Ok,
                "the mappings and the sub-area are made");
        if (tally.failed != 0) {
            log::error("vmaseal.test: FAILED -- setup");
            return 1;
        }
        ustl::Arc<mm::VmMapping> duplicate;
        ustl::Arc<mm::VmArea> duplicate_area;
        {
            Guard guard{root->get_lock()};
            auto const status = mm::VmMapping::create(root.as_ptr_mut(), PLAIN, PAGE_SIZE, PERMS,
                                                      make_vmo(PAGE_SIZE), 0, flags,
                                                      "duplicate mapping", &duplicate);
            verdict(tally, status == Status::AlreadyExists && !duplicate,
                    "failed tree insertion does not publish a mapping");
            verdict(tally,
                    mm::VmArea::create(PLAIN, PAGE_SIZE, PERMS, root.as_ptr_mut(),
                                       aspace.as_ptr_mut(), "duplicate area",
                                       &duplicate_area) == Status::AlreadyExists &&
                        !duplicate_area,
                    "failed tree insertion does not publish a sub-area");
        }
        verdict(tally,
                root->reserve(SEALED + PAGE_SIZE, PAGE_SIZE, flags, "overlap") ==
                    Status::AlreadyExists,
                "adopting existing entries cannot overlap a linked mapping");
        ustl::Arc<mm::VmMapping> found;
        verdict(tally,
                aspace->find_mapping(PLAIN, &found) == Status::Ok &&
                    found.as_ptr() == plain->as_ptr(),
                "the original mapping survives the rejected insertion");
        verdict(tally,
                root->protect(PLAIN, 0, flags) == Status::InvArgs &&
                    root->protect(root->last() + 1, PAGE_SIZE, flags) == Status::InvArgs &&
                    root->protect(PLAIN, PAGE_SIZE, mm::MmuFlags::User) == Status::InvArgs,
                "protect rejects empty ranges, ranges outside the area and invalid permissions");
        auto &plain_mapping = **plain;
        verdict(tally,
                plain_mapping.map(PLAIN, 0, mm::MapOptions::Resident) == Status::InvArgs &&
                    plain_mapping.map(PLAIN + PAGE_SIZE, PAGE_SIZE, mm::MapOptions::Resident) ==
                        Status::InvArgs &&
                    plain_mapping.protect(PLAIN, 0, flags) == Status::InvArgs &&
                    plain_mapping.protect(PLAIN + PAGE_SIZE, PAGE_SIZE, flags) == Status::InvArgs &&
                    plain_mapping.protect(PLAIN, PAGE_SIZE, mm::MmuFlags::User) ==
                        Status::InvArgs &&
                    plain_mapping.unmap(PLAIN, 0, mm::UnmapOptions::None) == Status::InvArgs &&
                    plain_mapping.unmap(PLAIN + PAGE_SIZE, PAGE_SIZE, mm::UnmapOptions::None) ==
                        Status::InvArgs,
                "a direct mapping operation validates its range and permissions");
        verdict(tally, aspace->soft_fault(PLAIN, mm::VmfCause::Absent) == Status::Ok,
                "fault lookup caches the plain mapping before teardown");
        auto inside =
            area->map_with_vmo(AREA + PAGE_SIZE, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                               mm::MapOptions::Sealed, "vmaseal.test inside");
        verdict(tally, inside.is_ok(), "a sealed mapping is made inside the sub-area");

        verdict(tally, root->unmap(SEALED, 2 * PAGE_SIZE) == Status::AccessDenied,
                "unmap of a sealed mapping is AccessDenied");
        verdict(tally, root->unmap(SEALED, PAGE_SIZE) == Status::AccessDenied,
                "trimming a sealed mapping is AccessDenied");
        verdict(tally,
                root->protect(SEALED, 2 * PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User) ==
                    Status::AccessDenied,
                "protect of a sealed mapping is AccessDenied");
        verdict(tally,
                sealed.unwrap()->unmap(SEALED, PAGE_SIZE, mm::UnmapOptions::None) ==
                    Status::AccessDenied,
                "unmap through the sealed mapping is AccessDenied");
        verdict(tally, sealed.unwrap()->protect(SEALED, PAGE_SIZE, flags) == Status::AccessDenied,
                "protect through the sealed mapping is AccessDenied");

        // From the last page of SEALED through PLAIN.
        verdict(tally, root->unmap(SEALED + PAGE_SIZE, 2 * PAGE_SIZE) == Status::AccessDenied,
                "an unmap that reaches a sealed mapping is refused");
        verdict(tally, plain.unwrap()->is_active(),
                "and it is refused whole: the plain mapping beside it is still there");

        verdict(tally, root->unmap(AREA, 4 * PAGE_SIZE) == Status::AccessDenied,
                "unmap of a sub-area with a sealed mapping inside is AccessDenied");
        verdict(tally, inside.unwrap()->is_active(), "and the sealed mapping inside survives it");

        verdict(tally,
                root->protect(PLAIN, PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User) ==
                    Status::Ok,
                "protect of the plain mapping is Ok");
        verdict(tally, root->unmap(PLAIN, PAGE_SIZE) == Status::Ok,
                "unmap of the plain mapping is Ok");
        verdict(tally, !plain.unwrap()->is_active(), "and the plain mapping is gone");
        verdict(tally, aspace->soft_fault(PLAIN, mm::VmfCause::Absent) == Status::NotFound,
                "the next fault cannot find the removed mapping in the address-space cache");
        verdict(tally,
                plain_mapping.map(PLAIN, PAGE_SIZE, mm::MapOptions::Resident) == Status::BadState &&
                    plain_mapping.protect(PLAIN, PAGE_SIZE, flags) == Status::BadState &&
                    plain_mapping.unmap(PLAIN, PAGE_SIZE, mm::UnmapOptions::None) ==
                        Status::BadState &&
                    root->destroy_child(plain_mapping) == Status::BadState,
                "a retained mapping cannot be changed or destroyed again after removal");

        check_protect_preflight(tally, *aspace);
        check_registration_undo(tally, *aspace);
#if MM_TEST_ALLOC_FAILURE
        check_aspace_allocation_failures(tally);
        check_allocation_failures(tally, *aspace);
        check_protect_allocation_failure(tally, *aspace);
        check_partial_map_failure(tally, *aspace);
        check_unmap_allocation_failure(tally, *aspace);
#endif

        // The sealed ones are in the address space when it goes.
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space, sealed mappings and all, can be destroyed");
        ustl::Arc<mm::VmArea> after_destroy;
        verdict(tally,
                root->create_subvma(AREA, PAGE_SIZE, PERMS, "after destroy", &after_destroy) ==
                        Status::BadState &&
                    root->protect(SEALED, PAGE_SIZE, flags) == Status::BadState &&
                    root->unmap(SEALED, PAGE_SIZE) == Status::BadState &&
                    aspace->find_mapping(SEALED, &found) == Status::BadState &&
                    aspace->destroy() == Status::BadState,
                "retained area and address-space references reject operations after teardown");
        {
            Guard guard{root->get_lock()};
            verdict(tally,
                    mm::VmArea::create(AREA, PAGE_SIZE, PERMS, root.as_ptr_mut(),
                                       aspace.as_ptr_mut(), "inactive parent",
                                       &after_destroy) == Status::BadState &&
                        !after_destroy,
                    "direct registration rejects an inactive parent");
        }

        if (tally.failed != 0) {
            log::error("vmaseal.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmaseal.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmaSealTest, "vmaseal.test", ours::vmaseal_test_command,
             "check that a sealed mapping refuses unmap and protect, and only that one");
