#include <ours/object/handle.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
// object::Handle is a Box-backed, factory-constructed type -- no public
// constructor and no copy -- so the typed syscall layer passes it by pointer:
// a by-value parameter could never be initialized from a caller's argument.
auto sys_thread_spawn(object::Handle *process, object::Handle *out) -> Status {
    return Status::Unimplemented;
}

auto sys_thread_start(object::Handle *thread, VirtAddr entry, VirtAddr stack, usize arg0,
                      usize arg1) -> Status {
    return Status::Unimplemented;
}

auto sys_thread_suspend(object::Handle *thread, object::Handle *out) -> Status {
    return Status::Unimplemented;
}

auto sys_thread_exit(object::Handle *thread, object::Handle *out) -> Status {
    return Status::Unimplemented;
}
} // namespace ours
