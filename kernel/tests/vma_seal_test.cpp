/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaSeal.RefusesUnmapAndProtectOnlyWhenSealed: a sealed mapping
// (MapOptions::Sealed) refuses to be unmapped or have its protection changed,
// and only that one.
//
// Why an in-kernel test: the refusals live in mm::VmArea::unmap() and
// protect(), and no program can reach protect() yet -- there is no
// vma_protect system call -- so only an in-kernel test can check that one. The
// unmap refusals a program can reach through vma_unmap, and system/app/shell
// checks them there too (check_vma_unmap()), along with what only a program
// can show: that a launcher's request for a sealed mapping is accepted and
// that the vDSO is refused without it. It is not a host test because an
// address space and its page tables only exist in a booted kernel.
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
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

    auto make_vmo(usize const size) -> ustl::Arc<mm::VmObject> {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER, size, mm::VmoFlags::Lazy, &vmo) !=
            Status::Ok) {
            return {};
        }
        return vmo;
    }

    auto check_protect_preflight(mm::VmAspace &aspace) -> void {
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto RW = READ | mm::MmuFlags::Writable;
        constexpr auto RX = READ | mm::MmuFlags::Executable;
        constexpr auto BASE = USER_ASPACE_BASE + 48_mb;
        auto root = aspace.root_vma();
        auto first = root->map_vmo(BASE, 3 * PAGE_SIZE, 0, RW, mm::VmaFlags::PermMask,
                                   make_vmo(3 * PAGE_SIZE), mm::MapOptions::Commit, "writable");
        auto neighbor = root->map_vmo(BASE + 3 * PAGE_SIZE, PAGE_SIZE, 0, READ, mm::VmaFlags::Read,
                                      make_vmo(PAGE_SIZE), mm::MapOptions::Lazy, "read ceiling");
        EXPECT_TRUE(first.is_ok() && neighbor.is_ok(),
                    "adjacent mappings have different permission ceilings");
        if (!first || !neighbor) {
            return;
        }
        auto const middle = BASE + PAGE_SIZE;
        PhysAddr pa{};
        mm::MmuFlags entry_flags{};
        EXPECT_TRUE(root->protect(BASE, 4 * PAGE_SIZE, RX) == Status::InvArgs &&
                        aspace.arch_aspace().query(middle, &pa, &entry_flags) == Status::Ok &&
                        !!(entry_flags & mm::MmuFlags::Writable),
                    "a neighbor's ceiling refuses the whole protect before write is removed");
        EXPECT_TRUE(root->protect(middle, PAGE_SIZE, READ) == Status::Ok &&
                        root->protect(middle, PAGE_SIZE, RW) == Status::Ok,
                    "restoring the middle page returns it to the neighboring protection");
        auto &mapping = **first;
        Guard guard{mapping.get_lock()};
        usize regions = 0;
        mapping.for_each_protection_range_locked([&](VirtAddr, usize, mm::MmuFlags) { ++regions; });
        EXPECT_TRUE(regions == 1, "equal protection spans coalesce after restoration");
    }

    auto check_registration_undo(mm::VmAspace &aspace) -> void {
        auto root = aspace.root_vma();
        constexpr auto RX = mm::MmuFlags::Readable | mm::MmuFlags::Executable | mm::MmuFlags::User;
        auto code = root->map_vmo(VirtAddr::Anywhere, PAGE_SIZE, 0, RX, mm::VmaFlags::PermMask,
                                  make_vmo(PAGE_SIZE),
                                  mm::MapOptions::Sealed | mm::MapOptions::Commit, "test code");
        auto const claimed = code.is_ok() && aspace.claim_vdso();
        EXPECT_TRUE(claimed, "the address space accepts one sealed code registration");
        if (!claimed) {
            return;
        }
        auto const address = (*code)->base();
        aspace.set_vdso(*code);
        EXPECT_TRUE(aspace.vdso().as_ptr() == code->as_ptr() && aspace.vdso_contains(address, 1) &&
                        !aspace.claim_vdso(),
                    "the registration retains its mapping and disallows a second claim");
        EXPECT_TRUE(root->destroy_child(**code) == Status::Ok && !aspace.vdso() &&
                        !aspace.vdso_contains(address, 1),
                    "undoing the mapping withdraws the address space's registration");
        auto const reclaimed = aspace.claim_vdso();
        EXPECT_TRUE(reclaimed, "withdrawal makes the claim reusable");
        if (reclaimed) {
            aspace.release_vdso();
        }
    }

#if MM_TEST_ALLOC_FAILURE
    auto check_protect_allocation_failure(mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 80_mb;
        constexpr usize PAGES = 32;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto RW = READ | mm::MmuFlags::Writable;
        auto root = aspace.root_vma();
        auto mapped = root->map_vmo(BASE, PAGES * PAGE_SIZE, 0, RW, mm::VmaFlags::PermMask,
                                    make_vmo(PAGES * PAGE_SIZE), mm::MapOptions::Commit,
                                    "protection allocation");
        EXPECT_TRUE(mapped.is_ok(), "a writable mapping for protection tree growth");
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
            EXPECT_TRUE(triggered && status == Status::OutOfMem &&
                            aspace.arch_aspace().query(at, &pa, &flags) == Status::Ok &&
                            !!(flags & mm::MmuFlags::Writable) &&
                            root->protect(at, PAGE_SIZE, READ) == Status::Ok,
                        "an allocation failure preserves the PTE until protect is retried");
        }
        EXPECT_TRUE(failures != 0, "the protection tree needed allocation while splitting");
    }

    auto check_unmap_allocation_failure(mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 112_mb;
        constexpr usize PAGES = 32;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto root = aspace.root_vma();
        auto mapped =
            root->map_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask,
                          make_vmo(PAGES * PAGE_SIZE), mm::MapOptions::Commit, "unmap allocation");
        EXPECT_TRUE(mapped.is_ok(), "a committed mapping for partial unmap");
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
                EXPECT_TRUE(status == Status::OutOfMem &&
                                aspace.arch_aspace().query(at, &pa, &flags) == Status::Ok,
                            "a failed partial unmap preserves the original PTE");
                status = root->unmap(at, PAGE_SIZE);
            }
            EXPECT_TRUE(status == Status::Ok &&
                            aspace.arch_aspace().query(at, &pa, &flags) == Status::NotFound &&
                            aspace.soft_fault(at, mm::VmFault::Cause::Absent) ==
                                Status::AccessDenied &&
                            aspace.arch_aspace().query(at - PAGE_SIZE, &pa, &flags) == Status::Ok,
                        "partial unmap removes the target translation and prevents a refault");
        }
        EXPECT_TRUE(failures != 0, "the unmap needed allocation while splitting");
    }

    auto check_aspace_allocation_failures() -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        auto const fails = [](AllocationSite const site, usize const skip, Status const expected) {
            ustl::Arc<mm::VmAspace> aspace;
            AllocationFailure failure(site, skip);
            auto const status = mm::VmAspace::create(mm::VmasKind::User, mm::VmasFlags::None,
                                                     "failed address space", &aspace);
            return status == expected && failure.triggered() && !aspace;
        };
        EXPECT_TRUE(fails(AllocationSite::Object, 0, Status::OutOfMem),
                    "address-space object allocation failure leaves no reference");
        EXPECT_TRUE(fails(AllocationSite::PageTable, 0, Status::NoMemory),
                    "a missing root table leaves the address space unpublished");
        EXPECT_TRUE(fails(AllocationSite::Object, 1, Status::OutOfMem),
                    "a missing root VMA unwinds an initialized page table");
    }

    auto check_allocation_failures(mm::VmAspace &aspace) -> void {
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
        EXPECT_TRUE(created == Status::Ok && vmo, "a fresh area for failed insertions");
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
        EXPECT_TRUE(failed_insert, "an allocation failure leaves the sub-area unpublished");

        bool failed_reservation = false;
        {
            AllocationFailure failure(AllocationSite::Heap);
            auto const status = parent->reserve(BASE, PAGE_SIZE, READ, "failed reservation");
            failed_reservation = status == Status::OutOfMem && failure.triggered();
        }
        EXPECT_TRUE(failed_reservation, "a failed reservation leaves the range available");
        bool failed_anon = false;
        {
            AllocationFailure failure(AllocationSite::Heap);
            auto mapped = parent->map_anon(BASE, PAGE_SIZE, READ, mm::MapOptions::Resident,
                                           "failed anonymous VMO");
            failed_anon =
                mapped.is_err() && mapped.unwrap_err() == Status::OutOfMem && failure.triggered();
        }
        EXPECT_TRUE(failed_anon, "an anonymous mapping propagates hierarchy allocation failure");

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
                auto mapped = parent->map_vmo(BASE, PAGE_SIZE, 0, READ, PERMS, vmo,
                                              mm::MapOptions::Commit, entry.description);
                failed =
                    mapped.is_err() && mapped.unwrap_err() == entry.error && failure.triggered();
            }
            ustl::Arc<mm::VmMapping> leftover;
            PhysAddr pa{};
            mm::MmuFlags flags{};
            EXPECT_TRUE(failed && aspace.find_mapping(BASE, &leftover) == Status::NotFound &&
                            aspace.arch_aspace().query(BASE, &pa, &flags) == Status::NotFound,
                        "{}", entry.description);
        }
        auto retried = parent->map_vmo(BASE, PAGE_SIZE, 0, READ, PERMS, vmo, mm::MapOptions::Commit,
                                       "mapping retry");
        EXPECT_TRUE(retried.is_ok(), "the same address can be mapped after every failed step");
    }

    auto check_partial_map_failure(mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 96_mb;
        constexpr usize PAGES = 34;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User;
        auto root = aspace.root_vma();
        auto vmo = make_vmo(PAGES * PAGE_SIZE);
        if (!vmo) {
            EXPECT_TRUE(false, "a VMO for partial map rollback is made");
            return;
        }
        bool failed;
        {
            // The first batch is installed and the next one has begun when the last page fails.
            AllocationFailure failure(AllocationSite::Frame, PAGES - 1);
            auto mapped = root->map_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask,
                                        vmo, mm::MapOptions::Commit, "failed partial map");
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
        EXPECT_TRUE(failed && unmapped && aspace.find_mapping(BASE, &leftover) == Status::NotFound,
                    "an allocation failure withdraws the mapping and every installed PTE");
        auto retried = root->map_vmo(BASE, PAGES * PAGE_SIZE, 0, READ, mm::VmaFlags::PermMask, vmo,
                                     mm::MapOptions::Commit, "partial map retry");
        EXPECT_TRUE(retried.is_ok(), "the same window can be remapped after partial failure");
    }

    auto check_demand_mapping_failure(mm::VmAspace &aspace) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 128_mb;
        constexpr auto RW = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto vmo = make_vmo(PAGE_SIZE);
        auto mapped = aspace.root_vma()->map_vmo(BASE, PAGE_SIZE, 0, RW, mm::VmaFlags::PermMask,
                                                 vmo, mm::MapOptions::Lazy, "failed demand map");
        EXPECT_TRUE(mapped.is_ok(), "a lazy mapping for page-table allocation failure");
        if (!mapped) {
            return;
        }
        bool failed;
        {
            AllocationFailure failure(AllocationSite::PageTable);
            auto const status = aspace.soft_fault(BASE, mm::VmFault::Cause::Write);
            failed = status == Status::NoMemory && failure.triggered();
        }
        PhysAddr pa{};
        mm::MmuFlags flags{};
        EXPECT_TRUE(failed && aspace.arch_aspace().query(BASE, &pa, &flags) == Status::NotFound &&
                        vmo->committed_pages(0, PAGE_SIZE) == 1,
                    "a failed page-table allocation leaves a committed page with no translation");
        EXPECT_TRUE(aspace.soft_fault(BASE, mm::VmFault::Cause::Write) == Status::Ok &&
                        aspace.arch_aspace().query(BASE, &pa, &flags) == Status::Ok &&
                        !!(flags & mm::MmuFlags::Writable),
                    "the next fault maps the committed page writable");
    }
#endif

    KTEST(VmaSeal, RefusesUnmapAndProtectOnlyWhenSealed) {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmaseal.test", &aspace);
            status != Status::Ok) {
            FAIL("vmaseal.test: could not create an address space: {}", static_cast<int>(status));
        }

        // Not const: ustl::Arc passes its own constness through to the area.
        auto root = aspace->root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto PERMS = mm::VmaFlags::PermMask;

        constexpr auto SEALED = USER_ASPACE_BASE + 16_mb;
        constexpr auto PLAIN = SEALED + 2 * PAGE_SIZE;
        constexpr auto AREA = USER_ASPACE_BASE + 32_mb;

        auto sealed = root->map_vmo(SEALED, 2 * PAGE_SIZE, 0, flags, PERMS, make_vmo(2 * PAGE_SIZE),
                                    mm::MapOptions::Sealed, "vmaseal.test sealed");
        auto plain = root->map_vmo(PLAIN, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                                   mm::MapOptions::Lazy, "vmaseal.test plain");
        ustl::Arc<mm::VmArea> area;
        auto const area_status = root->create_subvma(
            AREA, 4 * PAGE_SIZE, PERMS | mm::VmaFlags::CanMapSpecific, "vmaseal.test area", &area);
        EXPECT_TRUE(sealed.is_ok() && plain.is_ok() && area_status == Status::Ok,
                    "the mappings and the sub-area are made");
        if (!sealed || !plain || area_status != Status::Ok) {
            FAIL("vmaseal.test: FAILED -- setup");
        }
        ustl::Arc<mm::VmMapping> duplicate;
        ustl::Arc<mm::VmArea> duplicate_area;
        {
            Guard guard{root->get_lock()};
            auto const status = mm::VmMapping::create(root.as_ptr_mut(), PLAIN, PAGE_SIZE, PERMS,
                                                      make_vmo(PAGE_SIZE), 0, flags,
                                                      "duplicate mapping", &duplicate);
            EXPECT_TRUE(status == Status::AlreadyExists && !duplicate,
                        "failed tree insertion does not publish a mapping");
            EXPECT_TRUE(mm::VmArea::create(PLAIN, PAGE_SIZE, PERMS, root.as_ptr_mut(),
                                           aspace.as_ptr_mut(), "duplicate area",
                                           &duplicate_area) == Status::AlreadyExists &&
                            !duplicate_area,
                        "failed tree insertion does not publish a sub-area");
        }
        EXPECT_TRUE(root->reserve(SEALED + PAGE_SIZE, PAGE_SIZE, flags, "overlap") ==
                        Status::AlreadyExists,
                    "adopting existing entries cannot overlap a linked mapping");
        ustl::Arc<mm::VmMapping> found;
        EXPECT_TRUE(aspace->find_mapping(PLAIN, &found) == Status::Ok &&
                        found.as_ptr() == plain->as_ptr(),
                    "the original mapping survives the rejected insertion");
        EXPECT_TRUE(
            root->protect(PLAIN, 0, flags) == Status::InvArgs &&
                root->protect(root->last() + 1, PAGE_SIZE, flags) == Status::InvArgs &&
                root->protect(PLAIN, PAGE_SIZE, mm::MmuFlags::User) == Status::InvArgs,
            "protect rejects empty ranges, ranges outside the area and invalid permissions");
        auto &plain_mapping = **plain;
        EXPECT_TRUE(
            plain_mapping.map(PLAIN, 0, mm::MapOptions::Resident) == Status::InvArgs &&
                plain_mapping.map(PLAIN + PAGE_SIZE, PAGE_SIZE, mm::MapOptions::Resident) ==
                    Status::InvArgs &&
                plain_mapping.protect(PLAIN, 0, flags) == Status::InvArgs &&
                plain_mapping.protect(PLAIN + PAGE_SIZE, PAGE_SIZE, flags) == Status::InvArgs &&
                plain_mapping.protect(PLAIN, PAGE_SIZE, mm::MmuFlags::User) == Status::InvArgs &&
                plain_mapping.unmap(PLAIN, 0, mm::UnmapOptions::None) == Status::InvArgs &&
                plain_mapping.unmap(PLAIN + PAGE_SIZE, PAGE_SIZE, mm::UnmapOptions::None) ==
                    Status::InvArgs,
            "a direct mapping operation validates its range and permissions");
        EXPECT_TRUE(aspace->soft_fault(PLAIN, mm::VmFault::Cause::Absent) == Status::Ok,
                    "fault lookup caches the plain mapping before teardown");
        auto inside =
            area->map_vmo(AREA + PAGE_SIZE, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                          mm::MapOptions::Sealed, "vmaseal.test inside");
        EXPECT_TRUE(inside.is_ok(), "a sealed mapping is made inside the sub-area");

        EXPECT_TRUE(root->unmap(SEALED, 2 * PAGE_SIZE) == Status::AccessDenied,
                    "unmap of a sealed mapping is AccessDenied");
        EXPECT_TRUE(root->unmap(SEALED, PAGE_SIZE) == Status::AccessDenied,
                    "trimming a sealed mapping is AccessDenied");
        EXPECT_TRUE(
            root->protect(SEALED, 2 * PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User) ==
                Status::AccessDenied,
            "protect of a sealed mapping is AccessDenied");
        EXPECT_TRUE(sealed.unwrap()->unmap(SEALED, PAGE_SIZE, mm::UnmapOptions::None) ==
                        Status::AccessDenied,
                    "unmap through the sealed mapping is AccessDenied");
        EXPECT_TRUE(sealed.unwrap()->protect(SEALED, PAGE_SIZE, flags) == Status::AccessDenied,
                    "protect through the sealed mapping is AccessDenied");

        // From the last page of SEALED through PLAIN.
        EXPECT_TRUE(root->unmap(SEALED + PAGE_SIZE, 2 * PAGE_SIZE) == Status::AccessDenied,
                    "an unmap that reaches a sealed mapping is refused");
        EXPECT_TRUE(plain.unwrap()->is_active(),
                    "and it is refused whole: the plain mapping beside it is still there");

        EXPECT_TRUE(root->unmap(AREA, 4 * PAGE_SIZE) == Status::AccessDenied,
                    "unmap of a sub-area with a sealed mapping inside is AccessDenied");
        EXPECT_TRUE(inside.unwrap()->is_active(), "and the sealed mapping inside survives it");

        EXPECT_TRUE(root->protect(PLAIN, PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User) ==
                        Status::Ok,
                    "protect of the plain mapping is Ok");
        EXPECT_TRUE(root->unmap(PLAIN, PAGE_SIZE) == Status::Ok,
                    "unmap of the plain mapping is Ok");
        EXPECT_TRUE(!plain.unwrap()->is_active(), "and the plain mapping is gone");
        EXPECT_TRUE(aspace->soft_fault(PLAIN, mm::VmFault::Cause::Absent) == Status::NotFound,
                    "the next fault cannot find the removed mapping in the address-space cache");
        EXPECT_TRUE(
            plain_mapping.map(PLAIN, PAGE_SIZE, mm::MapOptions::Resident) == Status::BadState &&
                plain_mapping.protect(PLAIN, PAGE_SIZE, flags) == Status::BadState &&
                plain_mapping.unmap(PLAIN, PAGE_SIZE, mm::UnmapOptions::None) == Status::BadState &&
                root->destroy_child(plain_mapping) == Status::BadState,
            "a retained mapping cannot be changed or destroyed again after removal");

        check_protect_preflight(*aspace);
        check_registration_undo(*aspace);
#if MM_TEST_ALLOC_FAILURE
        check_aspace_allocation_failures();
        check_allocation_failures(*aspace);
        check_protect_allocation_failure(*aspace);
        check_partial_map_failure(*aspace);
        check_unmap_allocation_failure(*aspace);
        check_demand_mapping_failure(*aspace);
#endif

        // The sealed ones are in the address space when it goes.
        EXPECT_TRUE(aspace->destroy() == Status::Ok,
                    "the address space, sealed mappings and all, can be destroyed");
        ustl::Arc<mm::VmArea> after_destroy;
        EXPECT_TRUE(root->create_subvma(AREA, PAGE_SIZE, PERMS, "after destroy", &after_destroy) ==
                            Status::BadState &&
                        root->protect(SEALED, PAGE_SIZE, flags) == Status::BadState &&
                        root->unmap(SEALED, PAGE_SIZE) == Status::BadState &&
                        aspace->find_mapping(SEALED, &found) == Status::BadState &&
                        aspace->destroy() == Status::BadState,
                    "retained area and address-space references reject operations after teardown");
        {
            Guard guard{root->get_lock()};
            EXPECT_TRUE(mm::VmArea::create(AREA, PAGE_SIZE, PERMS, root.as_ptr_mut(),
                                           aspace.as_ptr_mut(), "inactive parent",
                                           &after_destroy) == Status::BadState &&
                            !after_destroy,
                        "direct registration rejects an inactive parent");
        }
    }
} // namespace
} // namespace ours
