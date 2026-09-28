/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <string_view>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <userabi/userboot.hpp>

namespace ours::syscall {
/// The calling process ends. All of it happens in the object layer
/// (ProcessDispatcher::exit()); this handler exists to be the entry, and to
/// keep the system-call layer from naming the task layer that finally ends
/// the thread (design 4.7).
///
/// No Status: the call does not come back, so there is no frame->ax to write
/// and no caller to read one. The generated shim knows it is noreturn and
/// writes nothing; the vDSO stub follows the `syscall` with ud2, so that a
/// kernel bug returning here faults at once rather than running on.
///
/// Zircon counterpart: sys_process_exit(), which calls
/// ProcessDispatcher::ExitCurrent(retcode).
auto process_exit(i64 const retcode) -> void {
    object::ProcessDispatcher::current()->exit(retcode);
}

/// TEMPORARY, and the one handler that breaks the rule every other one keeps:
/// a system call touches kernel/object and nothing below it. This one reaches
/// into userabi, and through it elfload, which maps frames and writes through
/// the physmap directly. It is allowed because it is the only way to start a
/// second program before a user-space loader exists -- that needs working VMO
/// reads and writes, a user libc and the binder moved out of the kernel
/// (design P1-C) -- and because it is fenced three ways: it takes only files
/// the kernel itself loaded from its own package, so ring 0 parses no ELF a
/// program supplied; the loading code is exactly the code that starts the
/// first program, not a second path; and it goes, with the GN edge that
/// allows it, when the loader lands (docs/repairs/260923/syscall.md).
auto process_spawn_boot(HandleValue const job, user_copy::UserInPtr<char const> const name,
                        usize const name_size, HandleValue const bootstrap) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();

    // The job is resolved before the bootstrap handle leaves the table, so
    // that the two may name the same handle: a caller handing its only job
    // handle to the child holds a reference to the job from here on, and the
    // removal below then takes nothing this call still needs. The other order
    // made that case fail with BadHandle, after the handle was already gone.
    ustl::Arc<object::JobDispatcher> parent;
    auto const resolved = table.get_dispatcher(job, Rights::ManageProcess, &parent);

    // The bootstrap handle leaves the caller's table whatever the outcome,
    // failure included, as a handle written into a channel is consumed in
    // Zircon: a caller that gets an error back does not have to guess
    // whether it still holds the handle -- it never does. It is dropped at
    // return, after the table's lock is long released.
    ustl::Box<object::Handle> handed;
    if (bootstrap != HANDLE_INVALID) {
        handed = table.remove(bootstrap);
        if (resolved == Status::Ok && !handed) {
            return Status::BadHandle;
        }
    }
    if (resolved != Status::Ok) {
        return resolved;
    }

    // Moving a handle into another process is what Transfer is for.
    if (handed && !handed->has_rights(Rights::Transfer)) {
        return Status::AccessDenied;
    }

    if (name_size == 0 || name_size > PROCESS_BOOT_NAME_MAX) {
        return Status::InvArgs;
    }
    char text[PROCESS_BOOT_NAME_MAX] = {};
    if (auto const status = name.copy_array_from_user(text, name_size); status != Status::Ok) {
        return status;
    }
    // A bare file name: a '/' is refused, which also keeps this from ever
    // naming vmours/image.elf -- the kernel itself -- or anything else under
    // a directory the package uses for its own parts.
    auto const view = std::string_view(text, name_size);
    if (view.find('/') != std::string_view::npos || view.find('\0') != std::string_view::npos) {
        return Status::InvArgs;
    }

    return userabi::launch(ustl::move(parent), view, ustl::move(handed));
}
} // namespace ours::syscall
