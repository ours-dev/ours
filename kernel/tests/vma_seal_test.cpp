/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaSeal.RefusesUnmapAndProtectOnlyWhenSealed: a sealed mapping
// (VmMapOption::Sealed) refuses to be unmapped or have its protection changed,
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
#include <ours/mm/cfg.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

#include <ktest/ktest.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

    auto make_vmo(usize const size) -> ustl::Arc<mm::VmObject> {
        ustl::Arc<mm::VmObjectPaged> vmo;
        if (mm::VmObjectPaged::create(mm::GAF_USER, size, mm::VmoFlags::Lazy, &vmo) != Status::Ok) {
            return {};
        }
        return vmo;
    }

    KTEST(VmaSeal, RefusesUnmapAndProtectOnlyWhenSealed) {
        ustl::Arc<mm::VmAspace> aspace;
        ASSERT_OK(mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                       mm::VmasKind::User, mm::VmasFlags::None, "vmaseal.test",
                                       &aspace),
                  "an address space of its own");

        // Not const: ustl::Arc passes its own constness through to the area.
        auto root = aspace->root_vma();
        auto const flags = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto PERMS = mm::VmaFlags::PermMask;

        constexpr auto SEALED = USER_ASPACE_BASE + 16_mb;
        constexpr auto PLAIN = SEALED + 2 * PAGE_SIZE;
        constexpr auto AREA = USER_ASPACE_BASE + 32_mb;

        auto sealed =
            root->map_with_vmo(SEALED, 2 * PAGE_SIZE, 0, flags, PERMS, make_vmo(2 * PAGE_SIZE),
                               mm::VmMapOption::Sealed, "vmaseal.test sealed");
        auto plain = root->map_with_vmo(PLAIN, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                                        mm::VmMapOption::None, "vmaseal.test plain");
        ustl::Arc<mm::VmArea> area;
        auto const area_status =
            root->create_subvma(AREA, 4 * PAGE_SIZE, PERMS | mm::VmaFlags::CanMapSpecific,
                                "vmaseal.test area", mm::VmMapOption::None, &area);
        ASSERT_TRUE(sealed.is_ok() && plain.is_ok() && area_status == Status::Ok,
                    "the mappings and the sub-area are made");

        auto inside =
            area->map_with_vmo(AREA + PAGE_SIZE, PAGE_SIZE, 0, flags, PERMS, make_vmo(PAGE_SIZE),
                               mm::VmMapOption::Sealed, "vmaseal.test inside");
        EXPECT_TRUE(inside.is_ok(), "a sealed mapping is made inside the sub-area");

        EXPECT_EQ(root->unmap(SEALED, 2 * PAGE_SIZE), Status::AccessDenied,
                  "unmap of a sealed mapping is AccessDenied");
        EXPECT_EQ(root->unmap(SEALED, PAGE_SIZE), Status::AccessDenied,
                  "trimming a sealed mapping is AccessDenied");
        EXPECT_EQ(root->protect(SEALED, 2 * PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User),
                  Status::AccessDenied, "protect of a sealed mapping is AccessDenied");

        // From the last page of SEALED through PLAIN.
        EXPECT_EQ(root->unmap(SEALED + PAGE_SIZE, 2 * PAGE_SIZE), Status::AccessDenied,
                  "an unmap that reaches a sealed mapping is refused");
        EXPECT_TRUE(plain.unwrap()->is_active(),
                    "and it is refused whole: the plain mapping beside it is still there");

        EXPECT_EQ(root->unmap(AREA, 4 * PAGE_SIZE), Status::AccessDenied,
                  "unmap of a sub-area with a sealed mapping inside is AccessDenied");
        EXPECT_TRUE(inside.unwrap()->is_active(), "and the sealed mapping inside survives it");

        EXPECT_OK(root->protect(PLAIN, PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User),
                  "protect of the plain mapping is Ok");
        EXPECT_OK(root->unmap(PLAIN, PAGE_SIZE), "unmap of the plain mapping is Ok");
        EXPECT_FALSE(plain.unwrap()->is_active(), "and the plain mapping is gone");

        // The sealed ones are in the address space when it goes.
        EXPECT_OK(aspace->destroy(),
                  "the address space, sealed mappings and all, can be destroyed");
    }

} // namespace
} // namespace ours
