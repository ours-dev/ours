/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmaseal.test`: a sealed mapping (VmMapOption::Sealed) refuses to be
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
#include <ours/mm/cfg.hpp>
#include <ours/mm/gaf.hpp>
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
        if (mm::VmObjectPaged::create(mm::GAF_USER, size, mm::VmoFlags::Lazy, &vmo) != Status::Ok) {
            return {};
        }
        return vmo;
    }

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

        constexpr auto SEALED = USER_ASPACE_BASE + MB(16);
        constexpr auto PLAIN = SEALED + 2 * PAGE_SIZE;
        constexpr auto AREA = USER_ASPACE_BASE + MB(32);

        Tally tally;

        auto sealed = root->map_with_vmo(mm::VmPlacement::at(SEALED), 2 * PAGE_SIZE, 0, flags,
                                         PERMS, make_vmo(2 * PAGE_SIZE), mm::VmMapOption::Sealed,
                                         "vmaseal.test sealed");
        auto plain =
            root->map_with_vmo(mm::VmPlacement::at(PLAIN), PAGE_SIZE, 0, flags, PERMS,
                               make_vmo(PAGE_SIZE), mm::VmMapOption::None, "vmaseal.test plain");
        ustl::Arc<mm::VmArea> area;
        auto const area_status = root->create_subvma(
            mm::VmPlacement::at(AREA), 4 * PAGE_SIZE, PERMS | mm::VmaFlags::CanMapSpecific,
            "vmaseal.test area", mm::VmMapOption::None, &area);
        verdict(tally, sealed.is_ok() && plain.is_ok() && area_status == Status::Ok,
                "the mappings and the sub-area are made");
        if (tally.failed != 0) {
            log::error("vmaseal.test: FAILED -- setup");
            return 1;
        }
        auto inside =
            area->map_with_vmo(mm::VmPlacement::at(AREA + PAGE_SIZE), PAGE_SIZE, 0, flags, PERMS,
                               make_vmo(PAGE_SIZE), mm::VmMapOption::Sealed, "vmaseal.test inside");
        verdict(tally, inside.is_ok(), "a sealed mapping is made inside the sub-area");

        verdict(tally, root->unmap(SEALED, 2 * PAGE_SIZE) == Status::AccessDenied,
                "unmap of a sealed mapping is AccessDenied");
        verdict(tally, root->unmap(SEALED, PAGE_SIZE) == Status::AccessDenied,
                "trimming a sealed mapping is AccessDenied");
        verdict(tally,
                root->protect(SEALED, 2 * PAGE_SIZE, mm::MmuFlags::Readable | mm::MmuFlags::User) ==
                    Status::AccessDenied,
                "protect of a sealed mapping is AccessDenied");

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

        // The sealed ones are in the address space when it goes.
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space, sealed mappings and all, can be destroyed");

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
