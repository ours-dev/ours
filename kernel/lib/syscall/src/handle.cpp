/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
auto handle_close(ours::Uandle const handle) -> Status {
    // Zircon's zx_handle_close() accepts ZX_HANDLE_INVALID as a no-op, and so
    // does this: it lets a program close a slot it may or may not have
    // filled without asking first, which is what every cleanup path wants.
    if (handle == ours::Uandle::Invalid) {
        return Status::Ok;
    }

    // Never null here: syscall_dispatch() refuses any trap that does not come
    // from a user process's vDSO before a handler runs.
    auto *const process = object::ProcessDispatcher::current();

    // remove() returns the object::Handle rather than destroying it, and it
    // goes out of scope at the end of this statement -- after the table's
    // lock was released inside remove(). Destroying the last handle to an
    // object runs its on_zero_handles(), which may take locks of its own; the
    // table's lock must not be held around that.
    auto closed = process->handle_table().remove(handle);
    if (!closed) {
        return Status::BadHandle;
    }
    return Status::Ok;
}

/// The checks and the copy are the table's (HandleTable::duplicate(), under
/// its lock); this handler does the out-parameter in the order of design
/// 4.1: the new handle's value is worked out and written to the program
/// first, and the handle goes into the table only after that write has
/// worked. A write that faults drops the duplicate here, after every lock,
/// and nothing was ever visible -- channel_create() publishes its two
/// ends the same way.
///
/// Zircon does it the other way round: its handler adds the handle and
/// returns the value, and the generated wrapper copies it out afterwards;
/// when that copy fails the handle is already in the table, so the wrapper
/// raises a HANDLE_LEAK policy exception on the thread
/// (kernel-wrappers.inc.tmpl). Writing first leaves nothing to leak.
auto handle_duplicate(ours::Uandle const handle, Rights const rights,
                      user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Box<object::Handle> duplicate;
    if (auto const status = table.duplicate(handle, rights, &duplicate); status != Status::Ok) {
        return status;
    }
    if (auto const status = out.copy_to_user(table.value_of(*duplicate)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(duplicate)));
    return Status::Ok;
}
} // namespace ours::syscall
