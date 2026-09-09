/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <ours/init.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/panic.hpp>

namespace ours::object {
namespace {
    /// The one job every process created so far belongs to.
    ///
    /// A file-scope Arc rather than a function-local static: a local static
    /// with a non-constant initialiser needs __cxa_guard_acquire, which this
    /// freestanding kernel does not link, and the guard would be paying for a
    /// race that cannot happen anyway (see the init hook below).
    ustl::Arc<JobDispatcher> ROOT_JOB;

    /// Built from an init hook rather than lazily on first use, for the same
    /// reason VmAspace::init_kernel_aspace() is: the boot sequence is the only
    /// thing that knows when the heap is up, and a lazy first-use path would
    /// have to be either thread-safe or documented as boot-only -- and the
    /// second is what the hook enforces for free.
    ///
    /// Vmm + 1, not Heap: the object is a plain heap allocation, but the first
    /// thing that will ever ask for it is process creation, which needs the
    /// VMM. Registering it any earlier only widens the window in which a
    /// half-built system could reach it.
    INIT_CODE
    auto init_root_job() -> void {
        ROOT_JOB = ustl::Arc<JobDispatcher>::adopt(new (mm::GAF_KERNEL) JobDispatcher());
        if (!ROOT_JOB) {
            panic("Failed to create the root job");
        }
    }
    GKTL_INIT_HOOK(JobRootInit, init_root_job, gktl::InitLevel::Vmm + 1);
} // namespace

auto JobDispatcher::root_job() -> ustl::Arc<Self> & {
    // Not a soft failure: every process belongs to a job, so a caller that
    // gets here before the hook above has run would go on to build a process
    // with no job at all rather than fail visibly.
    ASSERT(ROOT_JOB, "The root job has not been created yet");
    return ROOT_JOB;
}

} // namespace ours::object
