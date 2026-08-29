#include <gktl/counter.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/object/process_dispatcher.hpp>

GKTL_COUNTER(dispatcher_process_create_count, "dispatcher.process.create");
GKTL_COUNTER(dispatcher_process_destroy_count, "dispatcher.process.destroy");

namespace {
constexpr auto const PRIVATE_ASPACE_BASE = USER_ASPACE_BASE;

constexpr auto const PRIVATE_ASPACE_SIZE = USER_ASPACE_RESTRICTED_SIZE;

[[maybe_unused]]
constexpr auto const SHARED_ASPACE_BASE = PRIVATE_ASPACE_BASE + PRIVATE_ASPACE_SIZE + PAGE_SIZE;

[[maybe_unused]]
constexpr auto const SHARED_ASPACE_SIZE = USER_ASPACE_SIZE - USER_ASPACE_RESTRICTED_SIZE;

} // namespace

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