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
    /// it: Inspect, Duplicate, Transfer and ManageThread for any task,
    /// Enumerate as well for a process or a job (RIGHTS_TO_BIND_ALL) --
    /// the channel hands out what the task is doing, and handles to its
    /// threads, which a process's Enumerate is the proof of reaching.
    constexpr auto RIGHTS_TO_BIND =
        Rights::Inspect | Rights::Duplicate | Rights::Transfer | Rights::ManageThread;
    constexpr auto RIGHTS_TO_BIND_ALL = RIGHTS_TO_BIND | Rights::Enumerate;

    /// The end of an exception channel user space gets: it reads the
    /// exceptions and waits for them, and writes nothing. The kernel's end
    /// never reads, so a message written to it would sit there for as long as
    /// the task lives.
    constexpr auto RIGHTS_OF_THE_READING_END = Rights::Transfer | Rights::Wait | Rights::Read;

    /// Whether |handle| names a job. A job is a task too, so binding an
    /// exception channel to one is only not built yet (NotSupported), which
    /// is a different answer from "not a task" (BadType). Only asked once the
    /// process and thread lookups said BadType.
    auto names_a_job(object::HandleTable &table, ours::Uandle const handle) -> bool {
        ustl::Arc<object::JobDispatcher> job;
        return table.get_dispatcher(handle, Rights::None, &job) == Status::Ok;
    }
} // namespace

/// An exception channel for a process or a thread: a channel pair, the
/// kernel's end bound to the task's exceptionate, the other end, read-only,
/// to the caller.
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

    // A process first, then a thread. Holding the reference to the task is
    // what keeps its exceptionate alive until the bind is done.
    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<ProcessDispatcher> process;
    ustl::Arc<object::ThreadDispatcher> thread;
    Rights task_rights = Rights::None;
    auto status =
        table.get_dispatcher_with_rights(handle, RIGHTS_TO_BIND_ALL, &process, &task_rights);
    if (status == Status::BadType) {
        status = table.get_dispatcher_with_rights(handle, RIGHTS_TO_BIND, &thread, &task_rights);
        if (status == Status::BadType && names_a_job(table, handle)) {
            return Status::NotSupported;
        }
    }
    if (status != Status::Ok) {
        return status;
    }

    // The thread and process handles a handler can ask an exception for carry
    // the rights of |handle|, so the channel grants no more than its binder
    // had. A thread's channel hands out no process: it grants no way up the
    // task chain.
    object::Exceptionate *slot = nullptr;
    auto process_rights = Rights::None;
    if (process) {
        slot = &process->exceptionate();
        process_rights = task_rights;
    } else {
        slot = &thread->exceptionate();
    }

    KernelHandle<ChannelDispatcher> kernel_end;
    KernelHandle<ChannelDispatcher> reading_end;
    Rights ignored = Rights::None;
    if (auto const made = ChannelDispatcher::create(&kernel_end, &reading_end, &ignored);
        made != Status::Ok) {
        return made;
    }

    // Charged to the caller, as any handle it is given: it is the caller's
    // to keep or close.
    auto reading = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(reading_end)),
                                        RIGHTS_OF_THE_READING_END);
    if (!reading) {
        return Status::NoResource;
    }
    if (auto const copied = out.copy_to_user(table.value_of(*reading)); copied != Status::Ok) {
        return copied;
    }
    if (auto const bound = slot->set_channel(ustl::move(kernel_end), task_rights, process_rights);
        bound != Status::Ok) {
        return bound;
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

/// A handle to the thread that raised the exception |handle| names.
auto exception_get_thread(ours::Uandle const handle, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    return hand_out_task(
        handle, out, [](object::ExceptionDispatcher &exception, ustl::Box<object::Handle> *task) {
            return exception.make_thread_handle(task);
        });
}

/// A handle to the process of the thread that raised the exception |handle|
/// names. An exception read from a thread's channel gives none
/// (AccessDenied): that channel grants no way up the task chain.
auto exception_get_process(ours::Uandle const handle, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    return hand_out_task(
        handle, out, [](object::ExceptionDispatcher &exception, ustl::Box<object::Handle> *task) {
            return exception.make_process_handle(task);
        });
}
} // namespace ours::syscall
