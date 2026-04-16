#include <ours/object/process-dispatcher.hpp>
#include <ours/mem/cfg.hpp>
#include <ours/lockdep.hpp>

#include <gktl/counter.hpp>

GKTL_COUNTER(dispatcher_process_create_count, "dispatcher.process.create");
GKTL_COUNTER(dispatcher_process_destroy_count, "dispatcher.process.destroy");

namespace {
    CXX11_CONSTEXPR
    auto const kPrivateAspaceBase = USER_ASPACE_BASE;

    CXX11_CONSTEXPR
    auto const kPrivateAspaceSize = USER_ASPACE_RESTRICTED_SIZE;

    CXX11_CONSTEXPR
    auto const kSharedAspaceBase = kPrivateAspaceBase + kPrivateAspaceSize + PAGE_SIZE;

    CXX11_CONSTEXPR
    auto const kSharedAspaceSize = USER_ASPACE_SIZE - USER_ASPACE_RESTRICTED_SIZE;

} // namespace anonymous

namespace ours::object {
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