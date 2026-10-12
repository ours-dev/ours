/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <algorithm>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>

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
auto process_exit(i64 const retcode) -> void {
    object::ProcessDispatcher::current()->exit(retcode);
}

/// A new Initial process under |job|, and the two handles to it.
///
/// In order: the job with ManageProcess, the name cut to what fits, the process
/// and its root VMA made together -- one is no use without the other -- then
/// both handles published as channel_create publishes its two (design 4.1):
/// both values go out to the caller before either handle goes into the table,
/// so a bad out-pointer leaves nothing behind. If it fails there, the handles
/// die here, the process loses its last holder, and on_zero_handles() takes it
/// from Initial to Dead: nothing is left to clean up by hand.
///
/// The vDSO is not mapped here. Whoever starts the process maps it through
/// vma_map; the kernel only records the range once that mapping is made.
auto process_create(ours::Uandle const job, user_copy::UserInPtr<char const> const name,
                    usize const name_size, ProcessCreateOptions const options,
                    user_copy::UserOutPtr<ours::Uandle> const process_out,
                    user_copy::UserOutPtr<ours::Uandle> const root_vma_out) -> Status {
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
    char text[MAX_NAME_LEN] = {};
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

    auto _ = table.add(ustl::move(process_handle));
    auto _ = table.add(ustl::move(root_vma_handle));
    return Status::Ok;
}

/// Starts |process| with |thread| as its first thread, running at |entry| with
/// |stack|, and |arg1| -- a handle, which moves into the new process -- as the
/// handle it starts with.
///
/// The checks: the process and the thread each with Write, the thread must be
/// one of that process's own (AccessDenied), |arg1| taken out of the caller's
/// table and needing Transfer. Like the handles channel_write() takes, |arg1|
/// is gone from the caller's table whatever the outcome -- a caller that gets
/// an error back never has to guess whether it still holds it -- so it is
/// removed once everything else has been resolved, and after them a missing or
/// untransferable one does not stop it being removed. The process and thread
/// are resolved before it leaves the table, so |arg1| may name either: the
/// references held here outlive its removal.
///
/// The order of the errors is the order of the checks: |process|, then
/// |thread| (BadHandle, BadType, AccessDenied for a missing right), then an
/// |arg1| that names nothing (BadHandle), then that the thread is the
/// process's own (AccessDenied), then |arg1|'s Transfer (AccessDenied), then
/// what ProcessDispatcher::start() finds (BadState, InvArgs for an entry that
/// is not a user address, OutOfMem for the kernel thread).
auto process_start(ours::Uandle const process, ours::Uandle const thread, VirtAddr const entry,
                   VirtAddr const stack, ours::Uandle const arg1, usize const arg2) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();

    ustl::Arc<object::ProcessDispatcher> target;
    ustl::Arc<object::ThreadDispatcher> first;
    auto const process_status = table.get_dispatcher(process, Rights::Write, &target);
    auto const thread_status = table.get_dispatcher(thread, Rights::Write, &first);

    ustl::Box<object::Handle> handed;
    if (arg1 != ours::Uandle::Invalid) {
        handed = table.remove(arg1);
        if (process_status == Status::Ok && thread_status == Status::Ok && !handed) {
            return Status::BadHandle;
        }
    }
    if (process_status != Status::Ok) {
        return process_status;
    }
    if (thread_status != Status::Ok) {
        return thread_status;
    }

    if (&first->process() != target.as_ptr()) {
        return Status::AccessDenied;
    }
    if (handed && !handed->has_rights(Rights::Transfer)) {
        return Status::AccessDenied;
    }

    return target->start(*first, entry, stack, ustl::move(handed), arg2);
}

/// The arguments are checked before the handle, as Zircon checks them; the
/// count goes out only on success.
auto process_read_memory(ours::Uandle const process, VirtAddr const vaddr,
                         user_copy::UserOutPtr<u8> const buffer, usize const buffer_size,
                         user_copy::UserOutPtr<usize> const actual) -> Status {
    if (buffer.is_null() || buffer_size == 0 || buffer_size > MAX_PROCESS_READ_MEMORY) {
        return Status::InvArgs;
    }
    ustl::Arc<object::ProcessDispatcher> target;
    if (auto const status = object::ProcessDispatcher::current()->handle_table().get_dispatcher(
            process, Rights::Read | Rights::Write, &target);
        status != Status::Ok) {
        return status;
    }
    usize count = 0;
    if (auto const status = target->read_memory(vaddr, buffer, buffer_size, &count);
        status != Status::Ok) {
        return status;
    }
    if (actual.is_null()) {
        return Status::Ok;
    }
    return actual.copy_to_user(count);
}
} // namespace ours::syscall
