#include <syscall/handlers.hpp>

#include <algorithm>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
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

// A placeholder for the thread call that does not exist yet: it has no entry
// in syscalls.gni, so nothing declares or calls it. Its parameters are not
// the ABI's: a handler takes a handle as the ours::Uandle the generated shim
// passes, never as an object::Handle *.
auto thread_suspend(object::Handle *thread, object::Handle *out) -> Status {
    return Status::Unimplemented;
}
} // namespace ours::syscall
