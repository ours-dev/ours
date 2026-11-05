/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaUndo.UndoesOnlyItsOwnObject: the undo of a map() or an allocate() that
// could not be reported takes down the object that call made, and nothing
// else -- not what another holder of the region has put in the same place
// since.
//
// Why an in-kernel test: the interleaving it checks needs two holders of a
// region. One makes an object and fails to report it; before its undo runs,
// the other unmaps that object and puts one of its own in the same place. A
// program cannot reach that window -- the undo runs inside the call that made
// the object, before control returns -- so the test plays both parts, in that
// order, through the dispatcher calls the system calls make (vma_map,
// vma_allocate, vma_unmap).
//
// It builds an address space of its own, wraps its root region in a
// VmaDispatcher as a process's is, and checks that:
//
//   - a mapping, unmapped by "the other thread", which then maps its own --
//     the kernel places the lowest room that fits, so the second lands where
//     the first was -- is left in place by the first one's undo_map(), which
//     answers BadState;
//   - the same for a sub-region: allocate(), an unmap() of it through its
//     parent, a second allocate() landing at the same address, and
//     undo_allocate() of the first answering BadState with the second still
//     there;
//   - the undo of an object still there takes it down and frees its range,
//     a sealed mapping's too, which unmap() refuses;
//
// and then destroys the address space.
//
// With the undo by range it replaces -- unmap() of the first object's range
// -- all three fail: in the first two that undo removes the other thread's
// object, and in the third unmap() refuses the sealed mapping and takes
// nothing down.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/vma.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

    using UnmapScope = object::VmaDispatcher::UnmapScope;

    /// A readable mapping, which every mapping here is.
    constexpr auto READ = VmOptions::PermRead | VmOptions::CanMapRead;

    /// Whether something lies at |addr| in |region|: a mapping of a page
    /// placed there is refused with AlreadyExists. One that is not refused is
    /// taken down again at once.
    auto taken(object::VmaDispatcher &region, object::VmoDispatcher &vmo, VirtAddr const addr)
        -> bool {
        auto probe = region.map(READ | VmOptions::Specific, addr - region.vma()->base(), vmo, 0,
                                PAGE_SIZE, "vmaundo.test probe");
        if (probe.is_ok()) {
            auto _ = region.undo_map(ustl::move(probe).unwrap());
            return false;
        }
        return probe.unwrap_err() == Status::AlreadyExists;
    }

    auto check_mapping(object::VmaDispatcher &region, object::VmoDispatcher &vmo) -> void {
        auto first = region.map(READ, 0, vmo, 0, PAGE_SIZE, "vmaundo.test first");
        EXPECT_TRUE(first.is_ok(), "a mapping is made where the kernel places it");
        if (first.is_err()) {
            return;
        }
        auto const at = first->base();

        // The other thread: it unmaps the mapping through the region it
        // shares, and maps its own, which lands in the same place.
        EXPECT_TRUE(region.unmap(at, PAGE_SIZE, UnmapScope::Mapping) == Status::Ok,
                    "another holder of the region unmaps it");
        auto second = region.map(READ, 0, vmo, 0, PAGE_SIZE, "vmaundo.test second");
        EXPECT_TRUE(second.is_ok() && second->base() == at,
                    "and maps its own, which the kernel places at the same address");

        // The first call's undo, which used to be an unmap() of its range.
        EXPECT_TRUE(region.undo_map(ustl::move(first).unwrap()) == Status::BadState,
                    "the first call's undo_map() finds its mapping gone");
        EXPECT_TRUE(taken(region, vmo, at), "and leaves the other mapping in place");

        if (second.is_ok()) {
            EXPECT_TRUE(region.undo_map(ustl::move(second).unwrap()) == Status::Ok,
                        "undo_map() of a mapping still there takes it down");
            EXPECT_TRUE(!taken(region, vmo, at), "and frees its range");
        }
    }

    auto check_sub_region(object::VmaDispatcher &region, object::VmoDispatcher &vmo) -> void {
        object::KernelHandle<object::VmaDispatcher> first;
        auto rights = Rights::None;
        VirtAddr first_at{};
        EXPECT_TRUE(region.allocate(VmOptions::None, 0, PAGE_SIZE, first, rights, first_at) ==
                        Status::Ok,
                    "a sub-region is made where the kernel places it");
        if (!first.dispatcher()) {
            return;
        }

        // The other thread: it destroys the sub-region through the parent,
        // as a handle allowed to unmap subareas may, and reserves its own in its place.
        EXPECT_TRUE(region.unmap(first_at, PAGE_SIZE, UnmapScope::Subarea) == Status::Ok,
                    "another holder of the parent destroys it");
        object::KernelHandle<object::VmaDispatcher> second;
        VirtAddr second_at{};
        EXPECT_TRUE(region.allocate(VmOptions::None, 0, PAGE_SIZE, second, rights, second_at) ==
                            Status::Ok &&
                        second_at == first_at,
                    "and reserves its own, which the kernel places at the same address");

        EXPECT_TRUE(region.undo_allocate(*first.dispatcher()) == Status::BadState,
                    "the first call's undo_allocate() finds its sub-region gone");
        EXPECT_TRUE(taken(region, vmo, first_at), "and leaves the other sub-region in place");

        if (second.dispatcher()) {
            EXPECT_TRUE(region.undo_allocate(*second.dispatcher()) == Status::Ok,
                        "undo_allocate() of a sub-region still there takes it down");
            EXPECT_TRUE(!taken(region, vmo, first_at), "and frees its range");
        }
    }

    auto check_sealed(object::VmaDispatcher &region, object::VmoDispatcher &vmo) -> void {
        auto sealed =
            region.map(READ | VmOptions::Sealed, 0, vmo, 0, PAGE_SIZE, "vmaundo.test sealed");
        EXPECT_TRUE(sealed.is_ok(), "a sealed mapping is made");
        if (sealed.is_err()) {
            return;
        }
        auto const at = sealed->base();
        EXPECT_TRUE(region.unmap(at, PAGE_SIZE, UnmapScope::Mapping) == Status::AccessDenied,
                    "which unmap() refuses");
        EXPECT_TRUE(region.undo_map(ustl::move(sealed).unwrap()) == Status::Ok,
                    "and undo_map() takes it down, nobody having been told of it");
        EXPECT_TRUE(!taken(region, vmo, at), "and frees its range");
    }

    KTEST(VmaUndo, UndoesOnlyItsOwnObject) {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmaundo.test", &aspace);
            status != Status::Ok) {
            FAIL("vmaundo.test: could not create an address space: {}", to_string(status));
        }

        object::KernelHandle<object::VmaDispatcher> region;
        auto region_rights = Rights::None;
        object::KernelHandle<object::VmoDispatcher> vmo;
        auto vmo_rights = Rights::None;
        auto const made =
            object::VmaDispatcher::create(aspace->root_vma(), region, region_rights) ==
                Status::Ok &&
            object::VmoDispatcher::create_paged(PAGE_SIZE, vmo, vmo_rights) == Status::Ok;
        EXPECT_TRUE(made, "a region and a VMO are made");

        if (made) {
            check_mapping(*region.dispatcher(), *vmo.dispatcher());
            check_sub_region(*region.dispatcher(), *vmo.dispatcher());
            check_sealed(*region.dispatcher(), *vmo.dispatcher());
        }

        EXPECT_TRUE(aspace->destroy() == Status::Ok, "the address space is destroyed");
    }
} // namespace
} // namespace ours
