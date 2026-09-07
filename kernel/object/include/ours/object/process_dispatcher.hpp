/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PROCESS_DISPATCHER_HPP
#define OURS_OBJECT_PROCESS_DISPATCHER_HPP 1

#include <ktl/name.hpp>
#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
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

    static constexpr auto MAX_NAME_SIZE = 32;

    static auto create(ustl::Arc<JobDispatcher> &job, char const *name,
                       KernelHandle<Self> &process_out, Rights &process_rights,
                       KernelHandle<VmaDispatcher> &root_vma, Rights &root_vma_rights) -> Status;

    static auto create_shared(ustl::Arc<JobDispatcher> &job, char const *name,
                              KernelHandle<Self> &process_out, Rights &process_rights,
                              KernelHandle<VmaDispatcher> &root_vma, Rights &root_vma_rights)
        -> Status;

    auto attach_thread(ThreadDispatcher &thread) -> Status;

    auto detach_thread(ThreadDispatcher &thread) -> Status;

    /// The address space every thread of this process runs in.
    ///
    /// A raw pointer out of an owning Arc, because the callers are the fault
    /// path (Thread::Current::fault) and the context switch, both of which run
    /// while a thread of this process is current -- which is itself the proof
    /// that the process, and so this reference, is still alive. Handing them
    /// an Arc would make both paths bump a refcount they cannot need.
    FORCE_INLINE
    auto aspace() -> mm::VmAspace * {
        return m_aspace.as_ptr_mut();
    }

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("ProcessDispatcher");

    ProcessDispatcher(ustl::Arc<JobDispatcher> job, char const *name,
                      ustl::Arc<mm::VmAspace> aspace);

    Self *m_parent;

    State m_state;

    ktl::Name<MAX_NAME_SIZE> m_name;

    /// The job this process belongs to, held so the job outlives it.
    ///
    /// One-directional for now: the job does not keep a list of its processes.
    /// That list exists in Zircon to make a job killable and enumerable as a
    /// unit, and neither operation exists here yet -- adding the back-pointer
    /// before either does would be a list nothing ever reads.
    ustl::Arc<JobDispatcher> m_job;

    /// Unique address space per each process.
    ///
    /// Owning, and it has to be: the address space is created by create() and
    /// referenced from nowhere else, so a raw pointer here would leave the
    /// only Arc on create()'s stack and free the aspace on return. Zircon's
    /// ProcessDispatcher holds it the same way, as fbl::RefPtr<VmAspace>.
    ustl::Arc<mm::VmAspace> m_aspace;

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
    static Createdlist CREATED_PROCESS_LIST;
};

} // namespace ours::object

#endif // OURS_OBJECT_PROCESS_DISPATCHER_HPP
