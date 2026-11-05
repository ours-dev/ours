/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaPhysical.MapsPhysicalWindowUncached: object::VmaDispatcher::map() of a
// physical VMO installs the whole window, uncached; a paged VMO is mapped as
// before.
//
// Why an in-kernel test: a program can map a physical VMO (vmo_create_physical
// and vma_map) but cannot read a page-table entry back, and the cache
// attribute is a bit of the entry. The routing_checks read of a device's
// register shows the window is live; this shows what the entry says. It is not
// a host test because an address space and its page tables only exist in a
// booted kernel.
//
// It builds an address space of its own, wraps its root region in a
// VmaDispatcher as a process's is, and maps two pages of a physical VMO over
// a page above all RAM (never touched, so it does not matter whether a device
// answers there), and two pages of a paged VMO, both without MapRange:
//
//   - the physical window is in the page tables at once, each page on its own
//     frame, readable, writable and for the user, with the cache disabled;
//   - the paged VMO's pages carry no cache attribute: a physical object
//     is no reason to leave RAM cached.
//
// and then destroys the address space.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_page_splice_list.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/platform/mmio.hpp>
#include <ours/vma.hpp>

#include <algorithm>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

    /// The first page above all the RAM the kernel knows and every device it
    /// drives.
    auto page_above_ram() -> PhysAddr {
        auto top = PhysAddr{0};
        for (auto const &range: platform::kernel_ram()) {
            top = std::max<PhysAddr>(top, range.base + range.size);
        }
        for (auto const &range: platform::kernel_mmio()) {
            top = std::max<PhysAddr>(top, range.base + range.size);
        }
        return ustl::round_up(top, PAGE_SIZE);
    }

    /// What the page table holds for |va|.
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

    auto check_ownership(mm::VmObject &vmo) -> void {
        mm::VmPageSpliceList pages(1);
        EXPECT_TRUE(vmo.take_pages(0, PAGE_SIZE, &pages) == Status::Unsupported &&
                        vmo.supply_pages(0, PAGE_SIZE, &pages) == Status::Unsupported &&
                        pages.is_fresh(),
                    "a physical window cannot transfer ownership of frames");
        EXPECT_TRUE(vmo.pin_range(0, PAGE_SIZE) == Status::NotSupported &&
                        vmo.unpin_range(0, PAGE_SIZE) == Status::NotSupported &&
                        vmo.source_owner_id().is_none(),
                    "a physical window has neither counted VMO pins nor a pager");

        u64 word = 0x1234'5678;
        EXPECT_TRUE(vmo.read(&word, 0, sizeof(word)) == Status::Unsupported &&
                        vmo.write(&word, 0, sizeof(word)) == Status::Unsupported &&
                        vmo.store_u64(0, word) == Status::Unsupported && word == 0x1234'5678,
                    "generic VMO I/O does not touch physical memory");
        EXPECT_TRUE(vmo.read_user(user_copy::UserOutPtr<u8>{}, 0, sizeof(word)) ==
                            Status::Unsupported &&
                        vmo.write_user(user_copy::UserInPtr<u8 const>{}, 0, sizeof(word)) ==
                            Status::Unsupported,
                    "generic user-buffer copies also refuse physical memory");
    }

#if MM_TEST_ALLOC_FAILURE
    auto check_allocation_failures(mm::VmAspace &aspace, PhysAddr const phys) -> void {
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;
        constexpr auto BASE = USER_ASPACE_BASE + 64_mb;
        constexpr auto READ = mm::MmuFlags::Readable | mm::MmuFlags::User | mm::MmuFlags::Discache;
        auto root = aspace.root_vma();
        auto const map = [&](VirtAddr const at, usize const size) {
            return root->map_phys(phys, at, size, READ, mm::MapOptions::Resident,
                                  "physical allocation failure");
        };
        struct Case {
            AllocationSite site;
            usize pages;
            Status expected;
        };
        constexpr Case CASES[] = {
            {AllocationSite::Heap, 1, Status::OutOfMem},
            {AllocationSite::PageTable, 1, Status::NoMemory},
            {AllocationSite::PageTable, 32, Status::NoMemory},
        };
        auto at = BASE;
        for (auto const &test: CASES) {
            auto const size = test.pages * PAGE_SIZE;
            bool failed;
            {
                AllocationFailure failure(test.site);
                auto const mapping = map(at, size);
                failed = mapping.is_err() && mapping.unwrap_err() == test.expected &&
                         failure.triggered();
            }
            ustl::Arc<mm::VmMapping> leftover;
            EXPECT_TRUE(failed && !entry_at(aspace, at).present &&
                            aspace.find_mapping(at, &leftover) == Status::NotFound,
                        "a physical mapping failure leaves no PTE or registered mapping");
            auto retry = map(at, size);
            auto const last = entry_at(aspace, at + size - PAGE_SIZE);
            EXPECT_TRUE(retry.is_ok() && last.present && last.pa == phys + size - PAGE_SIZE &&
                            root->unmap(at, size) == Status::Ok,
                        "the physical window maps fully on retry");
            at += 2_mb;
        }
    }
#endif

    KTEST(VmaPhysical, MapsPhysicalWindowUncached) {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "physvmo.test", &aspace);
            status != Status::Ok) {
            FAIL("physvmo.test: could not create an address space: {}", to_string(status));
        }

        constexpr auto PAGES = usize{2};
        auto const phys = page_above_ram();
        object::KernelHandle<object::VmaDispatcher> region;
        auto region_rights = Rights::None;
        object::KernelHandle<object::VmoDispatcher> window;
        object::KernelHandle<object::VmoDispatcher> paged;
        auto vmo_rights = Rights::None;
        auto const made =
            phys != PhysAddr{0} &&
            object::VmaDispatcher::create(aspace->root_vma(), region, region_rights) ==
                Status::Ok &&
            object::VmoDispatcher::create_physical(phys, PAGES * PAGE_SIZE, window, vmo_rights) ==
                Status::Ok &&
            object::VmoDispatcher::create_paged(PAGES * PAGE_SIZE, paged, vmo_rights) == Status::Ok;
        EXPECT_TRUE(made, "a region, a physical VMO and a paged VMO are made");

        if (made) {
#if MM_TEST_ALLOC_FAILURE
            check_allocation_failures(*aspace, phys);
#endif
            auto vmo = window.dispatcher()->vmo();
            check_ownership(*vmo);
            PhysAddr first{};
            EXPECT_TRUE(
                vmo->lookup_contiguous(1, PAGE_SIZE, &first) == Status::Ok && first == phys + 1 &&
                    vmo->lookup_contiguous(0, 0, &first) == Status::InvArgs &&
                    vmo->lookup_contiguous(PAGES * PAGE_SIZE, 1, &first) == Status::OutOfRange,
                "a physical window resolves byte offsets and rejects invalid lengths");
            EXPECT_TRUE(vmo->commit_range(0, PAGES * PAGE_SIZE) == Status::Ok &&
                            vmo->commit_range(PAGES * PAGE_SIZE, PAGE_SIZE) == Status::OutOfRange,
                        "committing a physical window checks bounds without allocating frames");

            auto root = aspace->root_vma();
            constexpr auto READ =
                mm::MmuFlags::Readable | mm::MmuFlags::User | mm::MmuFlags::Discache;
            auto alias = root->map_phys(phys, VirtAddr::Anywhere, PAGE_SIZE, READ,
                                        mm::MapOptions::Resident, "physical read alias");
            EXPECT_TRUE(alias.is_ok(), "the kernel mapping helper installs a physical window");
            if (alias) {
                auto const address = (*alias)->base();
                auto const writable = READ | mm::MmuFlags::Writable;
                EXPECT_TRUE(root->protect(address, PAGE_SIZE, writable) == Status::Ok &&
                                aspace->soft_fault(address, mm::VmFault::Cause::Write) ==
                                    Status::Ok,
                            "a write fault can widen a physical mapping after protect");
                auto const entry = entry_at(*aspace, address);
                EXPECT_TRUE(entry.present && entry.pa == phys &&
                                (entry.flags & writable) == writable,
                            "widening preserves the physical frame and its uncached attribute");
            }

            constexpr auto OPTIONS = VmOptions::PermRead | VmOptions::PermWrite |
                                     VmOptions::CanMapRead | VmOptions::CanMapWrite;
            auto const physical = region.dispatcher()->map(OPTIONS, 0, *window.dispatcher(), 0,
                                                           PAGES * PAGE_SIZE, "physvmo.test");
            EXPECT_TRUE(physical.is_ok(), "the physical VMO is mapped, without MapRange");
            if (physical.is_ok()) {
                constexpr auto WANT = mm::MmuFlags::Readable | mm::MmuFlags::Writable |
                                      mm::MmuFlags::User | mm::MmuFlags::Discache;
                auto uncached = true;
                for (auto page = usize{0}; page < PAGES; ++page) {
                    auto const entry = entry_at(*aspace, physical->base() + page * PAGE_SIZE);
                    uncached = uncached && entry.present && entry.pa == phys + page * PAGE_SIZE &&
                               (entry.flags & WANT) == WANT;
                }
                EXPECT_TRUE(
                    uncached,
                    "each page of the window is in the page tables at once, on its own frame, "
                    "readable, writable, for the user, and uncached");
            }

            auto const ram =
                region.dispatcher()->map(OPTIONS | VmOptions::MapRange, 0, *paged.dispatcher(), 0,
                                         PAGES * PAGE_SIZE, "physvmo.test");
            EXPECT_TRUE(ram.is_ok(), "the paged VMO is mapped");
            if (ram.is_ok()) {
                auto cached = true;
                for (auto page = usize{0}; page < PAGES; ++page) {
                    auto const entry = entry_at(*aspace, ram->base() + page * PAGE_SIZE);
                    cached = cached && entry.present && !(entry.flags & mm::MmuFlags::Discache);
                }
                EXPECT_TRUE(cached, "the paged VMO's pages carry no cache attribute");
            }
        }

        EXPECT_TRUE(aspace->destroy() == Status::Ok, "the address space is destroyed");
    }
} // namespace
} // namespace ours
