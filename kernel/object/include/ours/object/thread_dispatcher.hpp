/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_THREAD_DISPATCHER_HPP
#define OURS_OBJECT_THREAD_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/task/types.hpp>

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/option.hpp>

namespace ours::object {
/// Rights a freshly created thread handle carries.
/// Zircon ZX_DEFAULT_THREAD_RIGHTS.
constexpr auto RIGHTS_DEFAULT_THREAD = Rights::Basic | Rights::Io | Rights::Property |
                                       Rights::Destroy | Rights::Signal | Rights::ManageThread;

using ThreadDispatcherBase = SoloDispatcher<ThreadDispatcher, RIGHTS_DEFAULT_THREAD, Signals::None>;

class ThreadDispatcher: public ThreadDispatcherBase {
    using Self = ThreadDispatcher;
    using Base = ThreadDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Thread);

    /// Guarded by get_lock().
    ///
    ///   Running  from creation until the thread starts to die; the kernel
    ///            thread may not have started yet.
    ///   Dying    killed, and the kernel thread told so, but not yet gone.
    ///   Dead     the kernel thread has exited and left the process; it is
    ///            no longer reachable from this object.
    ///
    /// Zircon's has Initial, Initialized and Suspended as well; nothing here
    /// distinguishes those yet.
    enum class State {
        Running,
        Dying,
        Dead,
    };

    enum class ParkedReason : u16 {};

    struct TaskState {
        VirtAddr entry = 0;
        VirtAddr stack = 0;
        usize arg1 = 0;
        usize arg2 = 0;
    };

    static auto spawn(ustl::Arc<ProcessDispatcher> process, char const *name,
                      KernelHandle<Self> *out) -> Status;

    /// The user thread the calling kernel thread runs on behalf of, or null
    /// on an ordinary kernel thread. See ProcessDispatcher::current().
    [[nodiscard]]
    static auto current() -> Self *;

    auto start(TaskState const &state) -> Status;

    auto resume() -> void;

    auto suspend() -> void;

    auto activate(bool suspend) -> Status;

    FORCE_INLINE
    auto process() -> ProcessDispatcher & {
        return m_process.as_ref_mut();
    }

    /// The read-only form. The syscall path reaches the process through a
    /// `const ThreadDispatcher &` -- it only asks where the vDSO is -- and
    /// without this it would have to cast the constness away to ask.
    FORCE_INLINE
    auto process() const -> ProcessDispatcher const & {
        return m_process.as_ref();
    }

    FORCE_INLINE
    auto mark_active() -> void {
        m_flags |= Flags::Active;
    }

    /// Marks this thread gone, as it leaves its process: Dead, and no longer
    /// pointing at a kernel thread that is about to be destroyed.
    ///
    /// Called by ProcessDispatcher::detach_thread() with the process's lock
    /// held; takes this thread's own lock after it, the order attach_thread()
    /// uses too.
    auto mark_dead() -> void;

  private:
    enum class Flags {
        Active = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    friend class task::Thread;

    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static auto trampoline(Self *) -> i32;

    ThreadDispatcher(ustl::Arc<ProcessDispatcher> process, char const *name);
    ~ThreadDispatcher() = default;

    auto on_exit() -> Status;

    auto on_resume() -> Status;

    auto on_suspend() -> Status;

    static constexpr auto MAGIC = gktl::magic("ThreadDispatcher");

    Flags m_flags;
    State m_state;
    ParkedReason m_parked_reason;
    /// The kernel thread this object runs as. Guarded by get_lock(); null
    /// once the thread has exited (mark_dead()), because the task::Thread is
    /// destroyed after its exit while this object may live on in handles.
    /// Zircon clears its core_thread_ at the same point (ResetCoreThread()).
    task::Thread *m_kernel_thread;

    ustl::Arc<ProcessDispatcher> m_process;
    ustl::Option<TaskState> m_user_entry;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);

  public:
    USTL_DECLARE_LIST(Self, List, ManagedOption);
}; // class ThreadDispatcher

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_THREAD_DISPATCHER_HPP