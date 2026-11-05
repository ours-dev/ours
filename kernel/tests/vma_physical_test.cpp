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
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/platform/mmio.hpp>
#include <ours/vma.hpp>

#include <algorithm>

#include <ktest/ktest.hpp>
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

    KTEST(VmaPhysical, MapsPhysicalWindowUncached) {
        ustl::Arc<mm::VmAspace> aspace;
        ASSERT_OK(mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                       mm::VmasKind::User, mm::VmasFlags::None, "physvmo.test",
                                       &aspace),
                  "an address space of its own");

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
                EXPECT_TRUE(uncached,
                            "each page of the window is in the page tables at once, on its own "
                            "frame, readable, writable, for the user, and uncached");
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

        EXPECT_OK(aspace->destroy(), "the address space is destroyed");
    }

} // namespace
} // namespace ours
