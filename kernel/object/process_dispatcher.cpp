#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>

#include <gktl/counter.hpp>
#include <heap/new.hpp>

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
                                     ustl::Arc<mm::VmAspace> aspace,
                                     ustl::Arc<HandleTable> handle_table)
    : Base()
    , m_parent(nullptr)
    , m_state(State::Initial)
    , m_name(name)
    , m_job(ustl::move(job))
    , m_aspace(ustl::move(aspace))
    , m_handle_table(ustl::move(handle_table))
    , m_vdso_code_base(0)
    , m_vdso_code_size(0) {}

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
    auto status = mm::VmAspace::create(PRIVATE_ASPACE_BASE, PRIVATE_ASPACE_SIZE, mm::VmasKind::User,
                                       mm::VmasFlags::None, name, &aspace);
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
    auto handle_table = ustl::Arc<HandleTable>::adopt(new (mm::GAF_KERNEL) HandleTable());
    if (!handle_table) {
        return Status::OutOfMem;
    }

    auto self = ustl::Arc<Self>::adopt(
        new (mm::GAF_USER) Self(job, name, ustl::move(aspace), ustl::move(handle_table)));
    if (!self) {
        return Status::OutOfMem;
    }

    process_rights = default_right();
    process_out = KernelHandle<Self>(ustl::move(self));

    DISPATCHER_PROCESS_CREATE_COUNT.add(1);
    return Status::Ok;
}

auto ProcessDispatcher::current() -> Self * {
    auto *const thread = ThreadDispatcher::current();
    if (thread == nullptr) {
        return nullptr;
    }
    return &thread->process();
}

/// Makes |thread| one of this process's running threads.
///
/// One lock, the process's own, over the three things that have to change
/// together: the state check, starting the thread, and the list. With the
/// list under a lock of its own -- as it used to be -- a kill walking the list
/// could run between the thread starting and the thread being listed, and
/// miss a thread that was already running; or a thread could be added to a
/// process that had begun dying. Under one lock the process is, to anyone
/// who takes that lock, either without the thread or with it running.
///
/// The first thread moves the process from Initial to Running; any later one
/// requires Running, so that nothing is added to a process that is dying.
///
/// Lock order: this process's lock, then the thread's (activate() takes it).
///
/// Zircon counterpart: ProcessDispatcher::AddInitializedThread().
auto ProcessDispatcher::attach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    Guard guard{get_lock()};

    auto const initial_thread = m_thread_list.empty();
    auto const expected = initial_thread ? State::Initial : State::Running;
    if (m_state != expected) {
        return Status::BadState;
    }

    thread.mark_active();
    if (auto const status = thread.activate(false); Status::Ok != status) {
        return status;
    }
    m_thread_list.push_back(thread);

    if (initial_thread) {
        m_state = State::Running;
    }
    return Status::Ok;
}

auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();
    return Status::Unimplemented;
}

} // namespace ours::object