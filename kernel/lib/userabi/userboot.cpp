/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vm_area_dispatcher.hpp>
#include <ours/status.hpp>

#include <userabi/userboot.hpp>

namespace userabi {
auto userboot_init() -> void {
    using namespace ours;
    using namespace ours::object;

    KernelHandle<ProcessDispatcher> process_handle;
    KernelHandle<VmAreaDispatcher> vma_handle;
    Rights process_rights;
    Rights vma_rights;
    auto const status =
        ProcessDispatcher::create(JobDispatcher::root_job(), "userboot", process_handle,
                                  process_rights, vma_handle, vma_rights);
    ASSERT(status == Status::Ok);

    // The process object and its root VMA now live in the KernelHandles
    // above. Registering them in a handle table for userboot to receive
    // awaits object::Handle::make() growing a definition (and the table
    // itself); nothing after this point exists yet.
}
} // namespace userabi
