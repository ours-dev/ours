/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// VmaWx.MapRefusesWriteAndExecute: object::VmaDispatcher::map() refuses a
// mapping that is both writable and executable, whoever asks -- W^X held for
// the kernel's own callers, not only for a program.
//
// Why an in-kernel test: vma_map refuses such a request itself, before it
// reaches the dispatcher, so no program can show that the dispatcher refuses
// it too. The callers this check is for are the kernel's own, its loader above
// all, which call the dispatcher directly. Not a host test because an address
// space and its dispatchers only exist in a booted kernel.
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
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/vma.hpp>

#include <ktest/ktest.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {

    KTEST(VmaWx, MapRefusesWriteAndExecute) {
        ustl::Arc<mm::VmAspace> aspace;
        ASSERT_OK(mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                       mm::VmasKind::User, mm::VmasFlags::None, "vmawx.test",
                                       &aspace),
                  "an address space of its own");

        object::KernelHandle<object::VmaDispatcher> region;
        auto region_rights = Rights::None;
        object::KernelHandle<object::VmoDispatcher> vmo;
        auto vmo_rights = Rights::None;
        auto const made =
            object::VmaDispatcher::create(aspace->root_vma(), region, region_rights) ==
                Status::Ok &&
            object::VmoDispatcher::create_paged(3 * PAGE_SIZE, vmo, vmo_rights) == Status::Ok;
        EXPECT_TRUE(made, "a region and a VMO are made");

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
            EXPECT_TRUE(rwx.is_err() && rwx.unwrap_err() == Status::InvArgs,
                        "read, write and execute is InvArgs");
            EXPECT_TRUE(map(VmOptions::PermRead | VmOptions::PermWrite, PAGE_SIZE).is_ok(),
                        "read and write, under the same ceiling, is Ok");
            EXPECT_TRUE(map(VmOptions::PermRead | VmOptions::PermExecute, 2 * PAGE_SIZE).is_ok(),
                        "read and execute, under the same ceiling, is Ok");
        }

        EXPECT_OK(aspace->destroy(), "the address space is destroyed");
    }

} // namespace
} // namespace ours
