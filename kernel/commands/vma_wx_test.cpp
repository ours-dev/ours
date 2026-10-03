/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmawx.test`: object::VmaDispatcher::map() refuses a mapping that is both
// writable and executable, whoever asks -- W^X held for the kernel's own
// callers, not only for a program.
//
// Why a console command: vma_map refuses such a request itself, before it
// reaches the dispatcher, so no program can show that the dispatcher refuses
// it too. The callers this check is for are the kernel's own, its loader
// above all, which call the dispatcher directly.
//
// It builds an address space of its own, wraps its root region in a
// VmaDispatcher as a process's is, and asks for one page of a paged VMO,
// every time under a ceiling that allows all three (every CanMap bit):
//
//   - read, write and execute: InvArgs;
//   - read and write, and read and execute: Ok -- a ceiling may allow both,
//     only the protection asked for is checked;
//
// and then destroys the address space.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>

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
            log::info("vmawx.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmawx.test: FAIL  {}", name);
    }

    auto vmawx_test_command(int, char **) -> int {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmawx.test", &aspace);
            status != Status::Ok) {
            log::error("vmawx.test: could not create an address space: {}", to_string(status));
            return 1;
        }

        Tally tally;
        object::KernelHandle<object::VmaDispatcher> region;
        auto region_rights = Rights::None;
        object::KernelHandle<object::VmoDispatcher> vmo;
        auto vmo_rights = Rights::None;
        auto const made =
            object::VmaDispatcher::create(aspace->root_vma(), region, region_rights) ==
                Status::Ok &&
            object::VmoDispatcher::create_paged(3 * PAGE_SIZE, vmo, vmo_rights) == Status::Ok;
        verdict(tally, made, "a region and a VMO are made");

        if (made) {
            constexpr auto CEILING =
                VmOptions::CanMapRead | VmOptions::CanMapWrite | VmOptions::CanMapExecute;
            // A page of the VMO each, so that the two that succeed do not ask
            // for the same pages.
            auto const map = [&](VmOptions const perms, u64 const offset) {
                return region.dispatcher()->map(perms | CEILING, 0, *vmo.dispatcher(), offset,
                                                PAGE_SIZE, "vmawx.test");
            };

            auto const rwx =
                map(VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute, 0);
            verdict(tally, rwx.is_err() && rwx.unwrap_err() == Status::InvArgs,
                    "read, write and execute is InvArgs");
            verdict(tally, map(VmOptions::PermRead | VmOptions::PermWrite, PAGE_SIZE).is_ok(),
                    "read and write, under the same ceiling, is Ok");
            verdict(tally, map(VmOptions::PermRead | VmOptions::PermExecute, 2 * PAGE_SIZE).is_ok(),
                    "read and execute, under the same ceiling, is Ok");
        }

        verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");

        if (tally.failed != 0) {
            log::error("vmawx.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmawx.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmaWxTest, "vmawx.test", ours::vmawx_test_command,
             "check that VmaDispatcher::map() refuses a writable and executable mapping");
