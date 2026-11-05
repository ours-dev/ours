/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmAddress.PlacesRequestsAndRefusesReservedAddresses: a VmArea places a
// request where the request says -- zero is a concrete address, and
// VirtAddr::Anywhere finds the first unoccupied page -- and refuses what the
// address-space layout does not allow: a base at a reserved value, a range
// that would cover one, and a fixed request a sub-area's ceiling does not
// permit.
//
// Why an in-kernel test: the address space has to be based at zero, which a
// program's is not (it starts at USER_ASPACE_BASE), and the rules belong to
// VmArea, which a program reaches only through its own address space's
// dispatcher. It is not a host test because an address space and its page
// tables only exist in a booted kernel.
//
// The checks, in order:
//
//   - an address space cannot be based at Anywhere or Invalid, cannot have a
//     range that intersects the reserved values, and cannot be given a size
//     that would cover them;
//   - in an address space of its own, based at zero and four pages long:
//     zero is taken as the concrete address zero, Anywhere finds the first
//     unoccupied page, and a fixed request over the first mapping is
//     AlreadyExists without replacing it;
//   - Anywhere also places a sub-area; a fixed request inside it is
//     AccessDenied because it lacks CanMapSpecific, and Anywhere still
//     places there;
//   - every reserved virtual value is InvArgs for a mapping and for a
//     sub-area, and the refused sub-area leaves no area behind;
//   - the mapping at zero is still the one found there afterwards.
//
// and then destroys the address space.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {

    auto check_placement(mm::VmAspace &aspace, ustl::Arc<mm::VmObject> const &vmo) -> void {
        auto root = aspace.root_vma();
        auto const map = [&vmo](mm::VmArea &area, VirtAddr const where) {
            return area.map_vmo(where, PAGE_SIZE, 0, mm::MmuFlags::Readable, mm::VmaFlags::PermMask,
                                vmo, mm::MapOptions::Lazy, "vmaddress.test");
        };
        auto const zero = map(*root, VirtAddr{});
        EXPECT_TRUE(zero.is_ok() && (*zero)->base() == VirtAddr{},
                    "zero requests the concrete address zero");

        auto const anywhere = map(*root, VirtAddr::Anywhere);
        EXPECT_TRUE(anywhere.is_ok() && (*anywhere)->base() == VirtAddr{PAGE_SIZE},
                    "Anywhere finds the first unoccupied page");

        auto const duplicate = map(*root, VirtAddr{});
        EXPECT_TRUE(duplicate.is_err() && duplicate.unwrap_err() == Status::AlreadyExists,
                    "a fixed overlapping request is refused");

        ustl::Arc<mm::VmArea> restricted;
        EXPECT_TRUE(root->create_subvma(VirtAddr::Anywhere, PAGE_SIZE, mm::VmaFlags::PermMask,
                                        "vmaddress.test restricted", &restricted) == Status::Ok,
                    "Anywhere also places a sub-area");
        if (restricted) {
            auto const fixed = map(*restricted, restricted->base());
            EXPECT_TRUE(fixed.is_err() && fixed.unwrap_err() == Status::AccessDenied,
                        "fixed placement still requires CanMapSpecific");
            auto const chosen = map(*restricted, VirtAddr::Anywhere);
            EXPECT_TRUE(chosen.is_ok() && (*chosen)->base() == restricted->base(),
                        "an area without CanMapSpecific still accepts Anywhere");
        }

        for (usize index = 0; index < 16; ++index) {
            auto const reserved = VIRT_ADDR_RESERVED_BASE + index;
            if (reserved == VirtAddr::Anywhere) {
                continue;
            }
            auto const bad = map(*root, reserved);
            EXPECT_TRUE(bad.is_err() && bad.unwrap_err() == Status::InvArgs,
                        "an unknown reserved address cannot become an aligned mapping");
            ustl::Arc<mm::VmArea> area;
            EXPECT_TRUE(root->create_subvma(reserved, PAGE_SIZE, mm::VmaFlags::PermMask,
                                            "vmaddress.test invalid", &area) == Status::InvArgs &&
                            !area,
                        "an unknown reserved address cannot place a sub-area");
        }

        ustl::Arc<mm::VmMapping> found;
        EXPECT_TRUE(zero.is_ok() && aspace.find_mapping(VirtAddr{}, &found) == Status::Ok &&
                        found.as_ptr() == zero->as_ptr(),
                    "rejected requests preserve the original mapping at zero");
    }

    auto check_range_rejections(mm::VmAspace &aspace, ustl::Arc<mm::VmObject> const &vmo) -> void {
        constexpr auto READ = mm::MmuFlags::Readable;
        constexpr auto WRITE = mm::MmuFlags::Writable;
        constexpr auto LAZY = mm::MapOptions::Lazy;
        constexpr auto INVALID_MODE = mm::MapOptions::Resident | mm::MapOptions::Commit;
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto root = aspace.root_vma();
        auto const map = [&](usize const size) {
            return root->map_vmo(VirtAddr::Anywhere, size, 0, READ, PERMS, vmo, LAZY,
                                 "invalid window");
        };
        auto const empty = map(0);
        auto const full = map(2 * PAGE_SIZE);
        EXPECT_TRUE(empty.is_err() && empty.unwrap_err() == Status::InvArgs && full.is_err() &&
                        full.unwrap_err() == Status::NotFound,
                    "placement rejects an empty request and one that cannot fit");

        auto const bad_anon = root->map_anon(VirtAddr::Anywhere, PAGE_SIZE, READ, INVALID_MODE,
                                             "invalid anonymous mode");
        auto const empty_anon =
            root->map_anon(VirtAddr::Anywhere, 0, READ, LAZY, "empty anonymous");
        auto const bad_phys = root->map_phys(PhysAddr{PAGE_SIZE}, VirtAddr::Anywhere, PAGE_SIZE,
                                             READ, INVALID_MODE, "invalid physical mode");
        auto const empty_phys = root->map_phys(PhysAddr{PAGE_SIZE}, VirtAddr::Anywhere, 0, READ,
                                               LAZY, "empty physical");
        EXPECT_TRUE(bad_anon.is_err() && bad_anon.unwrap_err() == Status::InvArgs &&
                        empty_anon.is_err() && empty_anon.unwrap_err() == Status::InvArgs &&
                        bad_phys.is_err() && bad_phys.unwrap_err() == Status::InvArgs &&
                        empty_phys.is_err() && empty_phys.unwrap_err() == Status::InvArgs,
                    "both mapping helpers reject empty ranges and conflicting modes");

        auto const end = root->base() + root->size();
        EXPECT_TRUE(root->reserve(end, PAGE_SIZE, READ, "outside") == Status::InvArgs &&
                        root->reserve(root->base(), 0, READ, "empty") == Status::InvArgs,
                    "reservation requires a nonempty range inside the area");

        ustl::Arc<mm::VmArea> read_only;
        auto const at = end - PAGE_SIZE;
        auto const created =
            root->create_subvma(at, PAGE_SIZE, mm::VmaFlags::Read, "read only", &read_only);
        EXPECT_TRUE(created == Status::Ok && read_only && read_only->base() == at,
                    "rejected operations leave the final page available");
        if (!read_only) {
            return;
        }
        ustl::Arc<mm::VmArea> wider;
        auto const bad_mapping = read_only->map_vmo(VirtAddr::Anywhere, PAGE_SIZE, 0, WRITE, PERMS,
                                                    vmo, LAZY, "invalid initial permissions");
        EXPECT_TRUE(bad_mapping.is_err() && bad_mapping.unwrap_err() == Status::InvArgs &&
                        read_only->create_subvma(VirtAddr::Anywhere, PAGE_SIZE, mm::VmaFlags::Write,
                                                 "invalid child permissions",
                                                 &wider) == Status::InvArgs,
                    "mappings and child areas cannot exceed the area's permission ceiling");
        EXPECT_TRUE(
            read_only->reserve(at, PAGE_SIZE, WRITE, "denied permission") == Status::InvArgs &&
                read_only->reserve(at, PAGE_SIZE, READ, "denied placement") == Status::AccessDenied,
            "reservation observes both the permission and placement ceiling");
    }

    auto check_reserved_layouts() -> void {
        ustl::Arc<mm::VmAspace> invalid;
        auto const create = [&invalid](VirtAddr const base, usize const size) {
            return mm::VmAspace::create(base, size, mm::VmasKind::User, mm::VmasFlags::None,
                                        "vmaddress.test invalid", &invalid);
        };
        EXPECT_TRUE(create(VirtAddr::Anywhere, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "an address-space base cannot be Anywhere");
        EXPECT_TRUE(create(VirtAddr::Invalid, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "an address-space base cannot be Invalid");
        auto const last_page = VirtAddr{~0_usize - (PAGE_SIZE - 1)};
        EXPECT_TRUE(create(last_page, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "a range intersecting the reserved values is refused without wrapping");
        EXPECT_TRUE(create(VirtAddr{}, ~0_usize) == Status::InvArgs && !invalid,
                    "a huge range cannot cover the reserved values");
        static_assert(KERNEL_ASPACE_BASE + KERNEL_ASPACE_SIZE <=
                      VirtAddr{~0_usize - (PAGE_SIZE - 1)});
    }

    KTEST(VmAddress, PlacesRequestsAndRefusesReservedAddresses) {
        check_reserved_layouts();

        ustl::Arc<mm::VmAspace> aspace;
        auto const status = mm::VmAspace::create(VirtAddr{}, 4 * PAGE_SIZE, mm::VmasKind::User,
                                                 mm::VmasFlags::None, "vmaddress.test", &aspace);
        if (status != Status::Ok) {
            FAIL("vmaddress.test: FAILED -- cannot create the zero-based test space");
        }
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER, PAGE_SIZE, mm::VmoFlags::Lazy, &vmo) ==
            Status::Ok) {
            check_placement(*aspace, vmo);
            check_range_rejections(*aspace, vmo);
        } else {
            EXPECT_TRUE(false, "the test VMO is created");
        }
        EXPECT_TRUE(aspace->destroy() == Status::Ok, "the test space is destroyed");
    }
} // namespace
} // namespace ours
