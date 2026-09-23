/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
auto sys_handle_close(HandleValue const handle) -> Status {
    // Zircon's zx_handle_close() accepts ZX_HANDLE_INVALID as a no-op, and so
    // does this: it lets a program close a slot it may or may not have
    // filled without asking first, which is what every cleanup path wants.
    if (handle == HANDLE_INVALID) {
        return Status::Ok;
    }

    // Never null here: syscall_dispatch() refuses any trap that does not come
    // from a user process's vDSO before a handler runs.
    auto *const process = object::ProcessDispatcher::current();

    // remove() returns the Handle rather than destroying it, and it goes out
    // of scope at the end of this statement -- after the table's lock was
    // released inside remove(). Destroying the last handle to an object runs
    // its on_zero_handles(), which may take locks of its own; the table's
    // lock must not be held around that.
    auto closed = process->handle_table().remove(handle);
    if (!closed) {
        return Status::BadHandle;
    }
    return Status::Ok;
}
} // namespace ours
