/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/exception_dispatcher.hpp>
#include <ours/object/exceptionate.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::ChannelDispatcher;
using object::KernelHandle;
using object::ProcessDispatcher;

namespace {
    /// What the handle to a task must grant to bind an exception channel to
    /// it. Zircon's: Inspect, Duplicate, Transfer and ManageThread for any
    /// task, Enumerate as well for a process or a job -- the channel hands
    /// out what the task is doing, and in time handles to its threads.
    constexpr auto RIGHTS_TO_BIND = Rights::Inspect | Rights::Duplicate | Rights::Transfer |
                                    Rights::ManageThread | Rights::Enumerate;

    /// The end of an exception channel user space gets: it reads the
    /// exceptions and waits for them, and writes nothing. The kernel's end
    /// never reads, so a message written to it would sit there for as long as
    /// the task lives. Zircon's kExceptionChannelRights.
    constexpr auto RIGHTS_OF_THE_READING_END = Rights::Transfer | Rights::Wait | Rights::Read;

    /// Whether |handle| names a task that is not a process: Zircon binds
    /// exception channels to threads and jobs as well, and those are only
    /// not supported here yet, which is a different answer from "not a
    /// task". Only asked once the process lookup said BadType.
    auto names_another_task(object::HandleTable &table, ours::Uandle const handle) -> bool {
        ustl::Arc<object::ThreadDispatcher> thread;
        if (table.get_dispatcher(handle, Rights::None, &thread) == Status::Ok) {
            return true;
        }
        ustl::Arc<object::JobDispatcher> job;
        return table.get_dispatcher(handle, Rights::None, &job) == Status::Ok;
    }
} // namespace

/// Zircon: sys_task_create_exception_channel() (kernel/lib/syscalls/exceptions.cc),
/// for a process: a channel pair, the kernel's end bound to the process's
/// exceptionate, the other end, read-only, to the caller.
///
/// The out-parameter in the order of design 4.1, as channel_create() does it:
/// the reading end's value is written to the program before anything is
/// bound or installed, so a write that faults leaves nothing behind. Then the
/// bind, which can still be refused (AlreadyBound, BadState): the refused
/// pair is dropped here, and the program holds a value that names nothing, as
/// after any failed call that writes one.
auto task_create_exception_channel(ours::Uandle const handle, ExceptionChannelOptions const options,
                                   user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    if (options != ExceptionChannelOptions::None) {
        return Status::InvArgs;
    }

    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<ProcessDispatcher> process;
    Rights task_rights = Rights::None;
    if (auto const status =
            table.get_dispatcher_with_rights(handle, RIGHTS_TO_BIND, &process, &task_rights);
        status != Status::Ok) {
        if (status == Status::BadType && names_another_task(table, handle)) {
            return Status::NotSupported;
        }
        return status;
    }

    KernelHandle<ChannelDispatcher> kernel_end;
    KernelHandle<ChannelDispatcher> reading_end;
    Rights ignored = Rights::None;
    if (auto const status = ChannelDispatcher::create(&kernel_end, &reading_end, &ignored);
        status != Status::Ok) {
        return status;
    }

    // Charged to the caller, as any handle it is given: it is the caller's
    // to keep or close.
    auto reading = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(reading_end)),
                                        RIGHTS_OF_THE_READING_END);
    if (!reading) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*reading)); status != Status::Ok) {
        return status;
    }
    // The thread and process handles a handler can ask an exception for carry
    // the rights of |handle|, so the channel grants no more than its binder
    // had. Zircon passes the task handle's rights for both, likewise.
    if (auto const status =
            process->exceptionate().set_channel(ustl::move(kernel_end), task_rights, task_rights);
        status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(reading));
    return Status::Ok;
}

namespace {
    /// The shared body of exception_get_thread and exception_get_process:
    /// |make| asks the exception for the handle; the value goes out before
    /// the handle is installed (design 4.1).
    template <typename Make>
    auto hand_out_task(ours::Uandle const handle, user_copy::UserOutPtr<ours::Uandle> const out,
                       Make const &make) -> Status {
        auto &table = ProcessDispatcher::current()->handle_table();
        ustl::Arc<object::ExceptionDispatcher> exception;
        if (auto const status = table.get_dispatcher(handle, Rights::None, &exception);
            status != Status::Ok) {
            return status;
        }
        ustl::Box<object::Handle> task;
        if (auto const status = make(*exception, &task); status != Status::Ok) {
            return status;
        }
        if (auto const status = out.copy_to_user(table.value_of(*task)); status != Status::Ok) {
            return status;
        }
        auto _ = table.add(ustl::move(task));
        return Status::Ok;
    }
} // namespace

/// Zircon: sys_exception_get_thread() (kernel/lib/syscalls/exceptions.cc).
auto exception_get_thread(ours::Uandle const handle, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    return hand_out_task(
        handle, out, [](object::ExceptionDispatcher &exception, ustl::Box<object::Handle> *task) {
            return exception.make_thread_handle(task);
        });
}

/// Zircon: sys_exception_get_process() (kernel/lib/syscalls/exceptions.cc).
auto exception_get_process(ours::Uandle const handle, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    return hand_out_task(
        handle, out, [](object::ExceptionDispatcher &exception, ustl::Box<object::Handle> *task) {
            return exception.make_process_handle(task);
        });
}
} // namespace ours::syscall
