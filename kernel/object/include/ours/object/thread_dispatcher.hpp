/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_THREAD_DISPATCHER_HPP
#define OURS_OBJECT_THREAD_DISPATCHER_HPP 1

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/task/types.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/option.hpp>

namespace ours::object {
/// Rights a freshly created thread handle carries.
constexpr auto RIGHTS_DEFAULT_THREAD = Rights::Signal | Rights::Io;

using ThreadDispatcherBase =
    SoloDispatcher<ThreadDispatcher, RIGHTS_DEFAULT_THREAD, Signals::Suspend>;

class ThreadDispatcher: public ThreadDispatcherBase {
    using Self = ThreadDispatcher;
    using Base = ThreadDispatcherBase;

  public:
    enum class State {
        Running,
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