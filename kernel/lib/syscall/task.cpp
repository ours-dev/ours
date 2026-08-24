#include <ours/types.hpp>
#include <ours/status.hpp>

namespace ours {
    auto sys_thread_spawn(Handle process, Handle &out) -> Status {
        return Status::Unimplemented;
    }

    auto sys_thread_start(Handle thread, VirtAddr entry, VirtAddr stack, usize arg0, usize arg1) -> Status {
        return Status::Unimplemented;
    }

    auto sys_thread_suspend(Handle thread, Handle &out) -> Status {
        return Status::Unimplemented;
    }

    auto sys_thread_exit(Handle thread, Handle &out) -> Status {
        return Status::Unimplemented;
    }

} // namespace ours::syscall