/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PROCESS_DISPATCHER_HPP
#define OURS_OBJECT_PROCESS_DISPATCHER_HPP 1

#include <ours/mm/types.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/task/thread.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
/// Rights a freshly created process handle carries.
constexpr auto RIGHTS_DEFAULT_PROCESS = Rights::Signal | Rights::Io;

using ProcessDispatcherBase =
    SoloDispatcher<ProcessDispatcher, RIGHTS_DEFAULT_PROCESS, Signals::Suspend>;

class ProcessDispatcher: public ProcessDispatcherBase {
    using Self = ProcessDispatcher;
    using Base = ProcessDispatcherBase;

  public:
    enum class State {
        Initial,
        Running,
        Dying,
        Dead,
    };

    static auto create(ustl::Arc<JobDispatcher> &job, char const *name,
                       KernelHandle<Self> &process_out, Rights &process_rights,
                       KernelHandle<VmAreaDispatcher> &root_vma, Rights &root_vma_rights) -> Status;

    static auto create_shared(ustl::Arc<JobDispatcher> &job, char const *name,
                              KernelHandle<Self> &process_out, Rights &process_rights,
                              KernelHandle<VmAreaDispatcher> &root_vma, Rights &root_vma_rights)
        -> Status;

    auto attach_thread(ThreadDispatcher &thread) -> Status;

    auto detach_thread(ThreadDispatcher &thread) -> Status;

    FORCE_INLINE auto aspace() -> mm::VmAspace * {
        return m_aspace;
    }

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("ProcessDispatcher");

    Self *m_parent;

    State m_state;

    /// Unique address space per each process.
    mm::VmAspace *m_aspace;

    /// All sub-process forked from this process.
    ustl::collections::intrusive::ListMemberHook<> m_sibling_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_sibling_hook, SiblingListOption);
    USTL_DECLARE_LIST(Self, SiblingList, SiblingListOption);
    SiblingList m_siblings;

    /// All sub-process forked from this process.
    ustl::collections::intrusive::ListMemberHook<> m_children_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_children_hook, ChildrenListOption);
    USTL_DECLARE_LIST(Self, ChildrenList, ChildrenListOption);
    ChildrenList m_children;

    /// All threads belong to this process.
    sync::Mutex m_thread_list_lock;
    ThreadDispatcher::List m_thread_list;

    /// For all of process.
    ustl::collections::intrusive::ListMemberHook<> m_created_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_created_hook, CreatedListOption);
    USTL_DECLARE_LIST(Self, Createdlist, CreatedListOption);
    static Createdlist s_created_process_list;
};

} // namespace ours::object

#endif // OURS_OBJECT_PROCESS_DISPATCHER_HPP
