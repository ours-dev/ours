#include <gktl/counter.hpp>
#include <ktl/new.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>

GKTL_COUNTER(DISPATCHER_PROCESS_CREATE_COUNT, "dispatcher.process.create");
GKTL_COUNTER(DISPATCHER_PROCESS_DESTROY_COUNT, "dispatcher.process.destroy");

namespace {
constexpr auto PRIVATE_ASPACE_BASE = USER_ASPACE_BASE;

constexpr auto PRIVATE_ASPACE_SIZE = USER_ASPACE_RESTRICTED_SIZE;

[[maybe_unused]]
constexpr auto SHARED_ASPACE_BASE = PRIVATE_ASPACE_BASE + PRIVATE_ASPACE_SIZE + PAGE_SIZE;

[[maybe_unused]]
constexpr auto SHARED_ASPACE_SIZE = USER_ASPACE_SIZE - USER_ASPACE_RESTRICTED_SIZE;

} // namespace

namespace ours::object {
ProcessDispatcher::ProcessDispatcher(ustl::Arc<JobDispatcher> job, char const *const name,
                                     ustl::Arc<mm::VmAspace> aspace)
    : Base()
    , m_parent(nullptr)
    , m_state(State::Initial)
    , m_name(name)
    , m_job(ustl::move(job))
    , m_aspace(ustl::move(aspace)) {}

auto ProcessDispatcher::create(ustl::Arc<JobDispatcher> &job, char const *const name,
                               KernelHandle<Self> &process_out, Rights &process_rights,
                               KernelHandle<VmaDispatcher> &root_vma_out, Rights &root_vma_rights)
    -> Status {
    // The restricted range and not the full user half. USER_ASPACE_SIZE runs
    // to the 2^47 canonical split; PRIVATE_ASPACE_SIZE stops at 2^46 and
    // leaves the upper half of the user range for the shared aspace
    // create_shared() will hand out, which only works if the private one is
    // carved to that boundary from the start rather than shrunk later.
    ustl::Arc<mm::VmAspace> aspace;
    auto status = mm::VmAspace::create(PRIVATE_ASPACE_BASE, PRIVATE_ASPACE_SIZE,
                                       mm::VmAspaceKind::User, mm::VmasFlags::None, name, &aspace);
    if (Status::Ok != status) {
        return status;
    }

    // The root VMA is the address space's own, created by VmAspace::init();
    // this only puts an object-layer face on it. That is the whole reason
    // create() hands back two objects: a process is not usable without a way
    // to map into it, and the only thing that may map is a handle.
    auto root_vma = aspace->root_vma();
    if (!root_vma) {
        return Status::InternalError;
    }

    status = VmaDispatcher::create(ustl::move(root_vma), root_vma_out, root_vma_rights);
    if (Status::Ok != status) {
        return status;
    }

    // Last, because it takes ownership of the aspace: everything above can
    // still fail and unwind by simply dropping Arcs, and after this line the
    // aspace has an owner that outlives this frame. A failure here leaves
    // root_vma_out holding a dispatcher, which the caller's KernelHandle
    // releases on its own -- and the aspace dies with it, since the root VMA
    // was the last thing referencing it.
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(job, name, ustl::move(aspace)));
    if (!self) {
        return Status::OutOfMem;
    }

    process_rights = default_right();
    process_out = KernelHandle<Self>(ustl::move(self));

    DISPATCHER_PROCESS_CREATE_COUNT.add(1);
    return Status::Ok;
}

auto ProcessDispatcher::attach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();
    thread.mark_active();

    thread.activate(false);

    Guard guard{m_thread_list_lock};
    m_thread_list.push_back(thread);
    return Status::Ok;
}

auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();
    return Status::Unimplemented;
}

} // namespace ours::object