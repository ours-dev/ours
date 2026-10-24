#include <syscall/handlers.hpp>

#include <algorithm>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/suspend_token_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
/// A new Initial thread of |process|, and a handle to it.
///
/// In order: the process with ManageThread, the name cut to what fits, then the
/// dispatcher and its handle, published as every handle is (design 4.1): the
/// value goes to the caller first, the handle into the table last, so a bad
/// out-pointer leaves nothing behind. Only the dispatcher is made -- the kernel
/// thread comes with process_start -- so a thread whose handle is closed
/// unstarted is simply destroyed.
///
/// No state check on the process: a Dead or Dying one refuses the thread when
/// it is started (BadState, ProcessDispatcher::attach_thread()), which is the
/// only place the answer is reliable, since the state can change between
/// here and there whatever is checked now.
auto thread_create(ours::Uandle const process, user_copy::UserInPtr<char const> const name,
                   usize const name_size, ThreadCreateOptions const options,
                   user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::ProcessDispatcher> owner;
    if (auto const status = table.get_dispatcher(process, Rights::ManageThread, &owner);
        status != Status::Ok) {
        return status;
    }

    if (options != ThreadCreateOptions::None) {
        return Status::InvArgs;
    }

    char text[MAX_NAME_LEN] = {};
    auto const length = std::min<usize>(name_size, sizeof(text) - 1);
    if (auto const status = name.copy_array_from_user(text, length); status != Status::Ok) {
        return status;
    }

    object::KernelHandle<object::ThreadDispatcher> thread;
    if (auto const status = object::ThreadDispatcher::create(ustl::move(owner), text, &thread);
        status != Status::Ok) {
        return status;
    }

    auto handle = object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(thread)),
                                       object::RIGHTS_DEFAULT_THREAD);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// Starts |thread|, a later thread of a running process, at |entry| with
/// |stack|, and |arg1| and |arg2| in its first two argument registers.
///
/// The thread with ManageThread, as Zircon's thread_start asks; then what
/// ThreadDispatcher::start() finds: InvArgs for an entry that is not a user
/// address, BadState unless the thread is Initial and its process Running.
/// The thread need not be the caller's own: a program may start a thread of a
/// process it holds the thread of, as Zircon allows. The first thread of a
/// process is process_start's, which hands it the process's first handle and
/// moves the process out of Initial.
auto thread_start(ours::Uandle const thread, VirtAddr const entry, VirtAddr const stack,
                  usize const arg1, usize const arg2) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::ThreadDispatcher> target;
    if (auto const status = table.get_dispatcher(thread, Rights::ManageThread, &target);
        status != Status::Ok) {
        return status;
    }
    return target->start({.entry = entry, .stack = stack, .arg1 = arg1, .arg2 = arg2}, false);
}

/// Ends the calling thread: it leaves its process on the way out
/// (ThreadDispatcher::on_exit()), and the last to leave ends the process.
auto thread_exit() -> void {
    object::ThreadDispatcher::exit_current();
}

/// Suspends the thread |task| names, and hands back the token that keeps it
/// suspended.
///
/// The thread with Write, as Zircon's task_suspend asks. A process is a task
/// too, and one this call does not suspend yet: with Write it answers
/// NotSupported, which says more than the BadType any other object gets.
/// Then what SuspendTokenDispatcher::create() finds: NotSupported for the
/// calling thread itself, BadState for a thread that is dying or dead. The
/// token is published as every handle is (design 4.1): its value goes to the
/// caller first, the handle into the table last. A token that dies on the
/// way -- no handle to be had, or a bad out-pointer -- resumes the thread as
/// it goes, so a failed call leaves no hold behind.
auto task_suspend(ours::Uandle const task, user_copy::UserOutPtr<ours::Uandle> const token)
    -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::ThreadDispatcher> thread;
    if (auto const status = table.get_dispatcher(task, Rights::Write, &thread);
        status != Status::Ok) {
        if (status != Status::BadType) {
            return status;
        }
        ustl::Arc<object::ProcessDispatcher> process;
        auto const as_process = table.get_dispatcher(task, Rights::Write, &process);
        return as_process == Status::Ok ? Status::NotSupported : as_process;
    }

    object::KernelHandle<object::SuspendTokenDispatcher> held;
    if (auto const status = object::SuspendTokenDispatcher::create(ustl::move(thread), &held);
        status != Status::Ok) {
        return status;
    }

    auto handle = object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(held)),
                                       object::RIGHTS_DEFAULT_SUSPEND_TOKEN);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = token.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// Ends the process |task| names, with TASK_RETCODE_SYSCALL_KILL.
///
/// The type first, then Destroy, as the order of errors asks (BadHandle >
/// BadType > AccessDenied, design 4.5): a process, a thread and a job are
/// tasks, anything else BadType. Only a process is killed. A thread answers
/// NotSupported, as in Zircon: one killed from outside dies wherever it is,
/// holding a lock of its program's, say, and its process goes on with that
/// lock held for good; a thread ends by its own thread_exit or with its
/// process. A job answers NotSupported too, until jobs have a life cycle to
/// end. The kill itself is ProcessDispatcher::kill(), which every exception
/// left unhandled already ends a process through.
auto task_kill(ours::Uandle const task) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::Dispatcher> object;
    auto rights = Rights::None;
    if (auto const status = table.get_untyped(task, &object, &rights); status != Status::Ok) {
        return status;
    }

    auto process = object.downcast<object::ProcessDispatcher>();
    auto const thread_or_job =
        object.downcast<object::ThreadDispatcher>() || object.downcast<object::JobDispatcher>();
    if (!process && !thread_or_job) {
        return Status::BadType;
    }
    if ((rights & Rights::Destroy) != Rights::Destroy) {
        return Status::AccessDenied;
    }
    if (!process) {
        return Status::NotSupported;
    }

    process->kill(TASK_RETCODE_SYSCALL_KILL);
    return Status::Ok;
}
} // namespace ours::syscall
