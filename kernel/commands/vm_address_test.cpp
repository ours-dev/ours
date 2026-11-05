/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    struct Tally {
        usize passed = 0;
        usize failed = 0;

        auto check(bool const holds, char const *const what) -> void {
            if (holds) {
                passed += 1;
                log::info("vmaddress.test: pass  {}", what);
            } else {
                failed += 1;
                log::error("vmaddress.test: FAIL  {}", what);
            }
        }
    };

    auto check_placement(Tally &tally, mm::VmAspace &aspace, ustl::Arc<mm::VmObject> const &vmo)
        -> void {
        auto root = aspace.root_vma();
        auto const map = [&vmo](mm::VmArea &area, VirtAddr const where) {
            return area.map_vmo(where, PAGE_SIZE, 0, mm::MmuFlags::Readable, mm::VmaFlags::PermMask,
                                vmo, mm::MapOptions::Lazy, "vmaddress.test");
        };
        auto const zero = map(*root, VirtAddr{});
        tally.check(zero.is_ok() && (*zero)->base() == VirtAddr{},
                    "zero requests the concrete address zero");

        auto const anywhere = map(*root, VirtAddr::Anywhere);
        tally.check(anywhere.is_ok() && (*anywhere)->base() == VirtAddr{PAGE_SIZE},
                    "Anywhere finds the first unoccupied page");

        auto const duplicate = map(*root, VirtAddr{});
        tally.check(duplicate.is_err() && duplicate.unwrap_err() == Status::AlreadyExists,
                    "a fixed overlapping request is refused");

        ustl::Arc<mm::VmArea> restricted;
        tally.check(root->create_subvma(VirtAddr::Anywhere, PAGE_SIZE, mm::VmaFlags::PermMask,
                                        "vmaddress.test restricted", &restricted) == Status::Ok,
                    "Anywhere also places a sub-area");
        if (restricted) {
            auto const fixed = map(*restricted, restricted->base());
            tally.check(fixed.is_err() && fixed.unwrap_err() == Status::AccessDenied,
                        "fixed placement still requires CanMapSpecific");
            auto const chosen = map(*restricted, VirtAddr::Anywhere);
            tally.check(chosen.is_ok() && (*chosen)->base() == restricted->base(),
                        "an area without CanMapSpecific still accepts Anywhere");
        }

        for (usize index = 0; index < 16; ++index) {
            auto const reserved = VIRT_ADDR_RESERVED_BASE + index;
            if (reserved == VirtAddr::Anywhere) {
                continue;
            }
            auto const bad = map(*root, reserved);
            tally.check(bad.is_err() && bad.unwrap_err() == Status::InvArgs,
                        "an unknown reserved address cannot become an aligned mapping");
            ustl::Arc<mm::VmArea> area;
            tally.check(root->create_subvma(reserved, PAGE_SIZE, mm::VmaFlags::PermMask,
                                            "vmaddress.test invalid", &area) == Status::InvArgs &&
                            !area,
                        "an unknown reserved address cannot place a sub-area");
        }

        ustl::Arc<mm::VmMapping> found;
        tally.check(zero.is_ok() && aspace.find_mapping(VirtAddr{}, &found) == Status::Ok &&
                        found.as_ptr() == zero->as_ptr(),
                    "rejected requests preserve the original mapping at zero");
    }

    auto check_range_rejections(Tally &tally, mm::VmAspace &aspace,
                                ustl::Arc<mm::VmObject> const &vmo) -> void {
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
        tally.check(empty.is_err() && empty.unwrap_err() == Status::InvArgs && full.is_err() &&
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
        tally.check(bad_anon.is_err() && bad_anon.unwrap_err() == Status::InvArgs &&
                        empty_anon.is_err() && empty_anon.unwrap_err() == Status::InvArgs &&
                        bad_phys.is_err() && bad_phys.unwrap_err() == Status::InvArgs &&
                        empty_phys.is_err() && empty_phys.unwrap_err() == Status::InvArgs,
                    "both mapping helpers reject empty ranges and conflicting modes");

        auto const end = root->base() + root->size();
        tally.check(root->reserve(end, PAGE_SIZE, READ, "outside") == Status::InvArgs &&
                        root->reserve(root->base(), 0, READ, "empty") == Status::InvArgs,
                    "reservation requires a nonempty range inside the area");

        ustl::Arc<mm::VmArea> read_only;
        auto const at = end - PAGE_SIZE;
        auto const created =
            root->create_subvma(at, PAGE_SIZE, mm::VmaFlags::Read, "read only", &read_only);
        tally.check(created == Status::Ok && read_only && read_only->base() == at,
                    "rejected operations leave the final page available");
        if (!read_only) {
            return;
        }
        ustl::Arc<mm::VmArea> wider;
        auto const bad_mapping = read_only->map_vmo(VirtAddr::Anywhere, PAGE_SIZE, 0, WRITE, PERMS,
                                                    vmo, LAZY, "invalid initial permissions");
        tally.check(bad_mapping.is_err() && bad_mapping.unwrap_err() == Status::InvArgs &&
                        read_only->create_subvma(VirtAddr::Anywhere, PAGE_SIZE, mm::VmaFlags::Write,
                                                 "invalid child permissions",
                                                 &wider) == Status::InvArgs,
                    "mappings and child areas cannot exceed the area's permission ceiling");
        tally.check(
            read_only->reserve(at, PAGE_SIZE, WRITE, "denied permission") == Status::InvArgs &&
                read_only->reserve(at, PAGE_SIZE, READ, "denied placement") == Status::AccessDenied,
            "reservation observes both the permission and placement ceiling");
    }

    auto check_reserved_layouts(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> invalid;
        auto const create = [&invalid](VirtAddr const base, usize const size) {
            return mm::VmAspace::create(base, size, mm::VmasKind::User, mm::VmasFlags::None,
                                        "vmaddress.test invalid", &invalid);
        };
        tally.check(create(VirtAddr::Anywhere, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "an address-space base cannot be Anywhere");
        tally.check(create(VirtAddr::Invalid, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "an address-space base cannot be Invalid");
        auto const last_page = VirtAddr{~0_usize - (PAGE_SIZE - 1)};
        tally.check(create(last_page, PAGE_SIZE) == Status::InvArgs && !invalid,
                    "a range intersecting the reserved values is refused without wrapping");
        tally.check(create(VirtAddr{}, ~0_usize) == Status::InvArgs && !invalid,
                    "a huge range cannot cover the reserved values");
        static_assert(KERNEL_ASPACE_BASE + KERNEL_ASPACE_SIZE <=
                      VirtAddr{~0_usize - (PAGE_SIZE - 1)});
    }

    auto vmaddress_test_command(int, char **) -> int {
        Tally tally;
        check_reserved_layouts(tally);

        ustl::Arc<mm::VmAspace> aspace;
        auto const status = mm::VmAspace::create(VirtAddr{}, 4 * PAGE_SIZE, mm::VmasKind::User,
                                                 mm::VmasFlags::None, "vmaddress.test", &aspace);
        if (status != Status::Ok) {
            log::error("vmaddress.test: FAILED -- cannot create the zero-based test space");
            return 1;
        }
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::ALLOC_USER, PAGE_SIZE, mm::VmoFlags::Lazy, &vmo) ==
            Status::Ok) {
            check_placement(tally, *aspace, vmo);
            check_range_rejections(tally, *aspace, vmo);
        } else {
            tally.check(false, "the test VMO is created");
        }
        tally.check(aspace->destroy() == Status::Ok, "the test space is destroyed");
        if (tally.failed != 0) {
            log::error("vmaddress.test: FAILED -- {} failures", tally.failed);
            return 1;
        }
        log::info("vmaddress.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmAddressTest, "vmaddress.test", ours::vmaddress_test_command,
             "check concrete zero, Anywhere, fixed placement and reserved virtual values");
