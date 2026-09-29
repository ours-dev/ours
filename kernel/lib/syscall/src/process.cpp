/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <algorithm>
#include <string_view>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
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

/// A new Initial process under |job|, and the two handles to it.
///
/// Zircon's sys_process_create(): the job with ManageProcess, the name cut to
/// what fits (Zircon truncates too, to ZX_MAX_NAME_LEN), the process and its
/// root VMA made together -- one is no use without the other -- then both
/// handles published as channel_create publishes its two (design 4.1): both
/// values go out to the caller before either handle goes into the table, so a
/// bad out-pointer leaves nothing behind. If it fails there, the handles die
/// here, the process loses its last holder, and on_zero_handles() takes it
/// from Initial to Dead: nothing is left to clean up by hand.
///
/// The vDSO is not mapped here. Whoever starts the process maps it through
/// vma_map, as Zircon's launcher does; the kernel only records the range once
/// that mapping is made.
auto process_create(HandleValue const job, user_copy::UserInPtr<char const> const name,
                    usize const name_size, ProcessCreateOptions const options,
                    user_copy::UserOutPtr<HandleValue> const process_out,
                    user_copy::UserOutPtr<HandleValue> const root_vma_out) -> Status {
    ustl::Arc<object::JobDispatcher> parent;
    auto &table = object::ProcessDispatcher::current()->handle_table();
    if (auto const status = table.get_dispatcher(job, Rights::ManageProcess, &parent);
        status != Status::Ok) {
        return status;
    }

    if (options != ProcessCreateOptions::None) {
        return Status::InvArgs;
    }

    // One byte short of the buffer, for the terminator the object's name
    // needs. A name longer than that is cut, not refused.
    char text[object::ProcessDispatcher::MAX_NAME_SIZE] = {};
    auto const length = std::min<usize>(name_size, sizeof(text) - 1);
    if (auto const status = name.copy_array_from_user(text, length); status != Status::Ok) {
        return status;
    }

    object::KernelHandle<object::ProcessDispatcher> process;
    Rights process_rights = Rights::None;
    object::KernelHandle<object::VmaDispatcher> root_vma;
    Rights root_vma_rights = Rights::None;
    if (auto const status = object::ProcessDispatcher::create(parent, text, process, process_rights,
                                                              root_vma, root_vma_rights);
        status != Status::Ok) {
        return status;
    }

    auto process_handle = object::Handle::make(
        object::KernelHandle<object::Dispatcher>(ustl::move(process)), process_rights);
    if (!process_handle) {
        return Status::NoResource;
    }
    auto root_vma_handle = object::Handle::make(
        object::KernelHandle<object::Dispatcher>(ustl::move(root_vma)), root_vma_rights);
    if (!root_vma_handle) {
        return Status::NoResource;
    }

    if (auto const status = process_out.copy_to_user(table.value_of(*process_handle));
        status != Status::Ok) {
        return status;
    }
    if (auto const status = root_vma_out.copy_to_user(table.value_of(*root_vma_handle));
        status != Status::Ok) {
        return status;
    }

    static_cast<void>(table.add(ustl::move(process_handle)));
    static_cast<void>(table.add(ustl::move(root_vma_handle)));
    return Status::Ok;
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
                        usize const name_size, HandleValue const bootstrap,
                        user_copy::UserOutPtr<HandleValue> const root_vma) -> Status {
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

    // The child's root VMA comes back to the caller, who writes it into the
    // child's processargs next (VmaRoot). Value out, then into the table, as
    // for every handle a call hands back (design 4.1). The child is already
    // running by then: if the value cannot be written, the caller gets the
    // error, never writes a message, and the child -- which waits for one --
    // ends when the caller closes its end of the channel.
    ustl::Box<object::Handle> region;
    if (auto const status = userabi::launch(ustl::move(parent), view, ustl::move(handed), &region);
        status != Status::Ok) {
        return status;
    }
    if (auto const status = root_vma.copy_to_user(table.value_of(*region)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(region)));
    return Status::Ok;
}
} // namespace ours::syscall
