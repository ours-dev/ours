/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PROCESS_DISPATCHER_HPP
#define OURS_OBJECT_PROCESS_DISPATCHER_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/exceptionate.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/object_info.hpp>
#include <ours/syscall.hpp>
#include <ours/task/thread.hpp>

#include <ktl/name.hpp>
#include <ktl/vec.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
/// Rights a freshly created process handle carries.
constexpr auto RIGHTS_DEFAULT_PROCESS = Rights::Basic | Rights::Io | Rights::Property |
                                        Rights::Enumerate | Rights::Destroy | Rights::Signal |
                                        Rights::ManageProcess | Rights::ManageThread;

using ProcessDispatcherBase =
    SoloDispatcher<ProcessDispatcher, RIGHTS_DEFAULT_PROCESS, Signals::None>;

class ProcessDispatcher: public ProcessDispatcherBase {
    using Self = ProcessDispatcher;
    using Base = ProcessDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Process);

    enum class State {
        Initial,
        Running,
        Dying,
        Dead,
    };

    static auto create(ustl::Arc<JobDispatcher> &job, char const *name,
                       KernelHandle<Self> &process_out, Rights &process_rights,
                       KernelHandle<VmaDispatcher> &root_vma, Rights &root_vma_rights) -> Status;

    static auto create_shared(ustl::Arc<JobDispatcher> &job, char const *name,
                              KernelHandle<Self> &process_out, Rights &process_rights,
                              KernelHandle<VmaDispatcher> &root_vma, Rights &root_vma_rights)
        -> Status;

    /// The process the calling thread belongs to, or null on a kernel thread.
    ///
    /// The object layer's answer to "who is asking", so that a system call
    /// never has to walk task::Thread itself (lib/syscall touches objects
    /// only). Raw, for the same reason as aspace(): the caller is running on
    /// a thread of this process, which is itself the proof it is alive.
    [[nodiscard]]
    static auto current() -> Self *;

    /// The name the process was created with, for diagnostics.
    [[nodiscard]]
    FORCE_INLINE
    auto name() const -> char const * {
        return m_name.data();
    }

    /// The handles this process holds.
    [[nodiscard]]
    FORCE_INLINE
    auto handle_table() -> HandleTable & {
        return m_handle_table.as_ref_mut();
    }

    /// The account every handle this process creates is charged to
    /// (HANDLE_MAX_PER_PROCESS); a handle takes its own reference to it.
    [[nodiscard]]
    FORCE_INLINE
    auto handle_account() const -> ustl::Arc<HandleAccount> {
        return m_handle_account;
    }

    /// The process's exception channel slot: where task_create_exception_channel
    /// binds a channel, and where an exception of any of its threads is sent.
    [[nodiscard]]
    FORCE_INLINE
    auto exceptionate() -> Exceptionate & {
        return m_exceptionate;
    }

    /// Starts this process: |thread|, which must be one of its own and not
    /// started, becomes its first thread and runs at |entry| with |stack|,
    /// |arg1| in the first argument register as a handle value, and |arg2|
    /// in the second. The process goes Initial -> Running with it.
    ///
    /// |arg1| is the handle the new process starts with (empty for none): it is
    /// put into this process's handle table, where its value is known, and
    /// only then is the thread started -- once it is runnable it may use that
    /// value at once, so the handle must already be there. |arg1| is consumed
    /// whatever the answer: if the start fails, the handle is taken out of the
    /// table again and closed, here, so a failed start leaves the process
    /// exactly as it was and the caller, who no longer has the handle, cannot
    /// leave one behind either.
    ///
    /// BadState when the process is not Initial or has no vDSO registered
    /// (VmAspace::has_vdso()): a process that could not make one system call
    /// is not started. Anything thread.start() answers otherwise.
    auto start(ThreadDispatcher &thread, VirtAddr entry, VirtAddr stack, ustl::Box<Handle> arg1,
               usize arg2) -> Status;

    /// Makes |thread| one of this process's running threads: the first one,
    /// which takes the process from Initial to Running (|first_thread|), or a
    /// later one, which needs it Running. Called by ThreadDispatcher::start().
    auto attach_thread(ThreadDispatcher &thread, bool first_thread) -> Status;

    /// Ends this process with |retcode|: it starts dying, and every thread
    /// in it is told to die. Returns at once; the threads exit on their own
    /// way back to ring 3, and the process object goes when the last
    /// reference to it does.
    ///
    /// Only the first kill's retcode counts. A process that is already dying
    /// keeps the reason it is dying for.
    auto kill(i64 retcode) -> void;

    /// The last handle to this process is gone. A process that was never
    /// started can never be: Initial goes straight to Dead and is torn down,
    /// so a launcher that fails between process_create and process_start does
    /// not leave an address space -- with the vDSO already mapped in it --
    /// that nothing will ever destroy. Any other state is left alone: a
    /// running process is kept alive by its own threads, not by handles.
    auto on_zero_handles() -> void override;

    /// The calling thread's process ends with |retcode|, and the calling
    /// thread with it; see the definition. Called on a thread of this process.
    [[noreturn]]
    auto exit(i64 retcode) -> void;

    auto detach_thread(ThreadDispatcher &thread) -> Status;

    /// This process's state as object_get_info reports it for
    /// InfoTopic::Process.
    ///
    /// The return code, the state and the start time are copied in one hold
    /// of the lock that guards them. kill() writes the return code and the
    /// state together, so two holds could pair Exited with the code from
    /// before the kill. The flags are worked out from the copied state after
    /// the lock is released.
    [[nodiscard]]
    auto get_info() -> InfoProcess;

    /// The records object_get_info reports for InfoTopic::ProcessMaps: the
    /// address space first, then every area and mapping, depth first. At most
    /// |max| of them go into |records|, and how many there are in all into
    /// |avail|. BadState once the address space is destroyed, NoMemory if the
    /// records cannot be had.
    ///
    /// Any thread may ask, about any process: it reads the space through this
    /// dispatcher's own reference to it, not through the running thread's.
    /// The space's lock is held only to walk it, never across an allocation
    /// or a user copy: the records are counted, allocated without the lock,
    /// then filled. A copy under it would deadlock a process asking about
    /// itself, since the lock is not recursive, and two processes asking
    /// about each other.
    [[nodiscard]]
    auto get_maps(usize max, ktl::Vec<InfoMaps> *records, usize *avail) -> Status;

    /// Copies this process's memory from |va| into |out|, at most |size|
    /// bytes and never past the end of the mapping that holds |va|, which may
    /// make it fewer; how many into |actual|. Read through the mapping's
    /// object, not this process's page tables, so the mapping's protection
    /// does not matter and nothing of the process is faulted in but pages
    /// that are read.
    ///
    /// NotFound when no mapping holds |va|, BadState once the address space
    /// is gone, InvArgs when |out| cannot take the bytes, OutOfRange when the
    /// mapping runs past its object's end, NotSupported for a mapping of
    /// physical memory. Any thread may ask, about any process.
    [[nodiscard]]
    auto read_memory(VirtAddr va, user_copy::UserOutPtr<u8> out, usize size, usize *actual)
        -> Status;

    /// Whether the process has not been started and no handle names it yet:
    /// its creator is still building it, and its job does not list it
    /// (JobDispatcher::get_process_koids()). Takes the process lock, so a
    /// job asks it under its own lock, in the Job -> Process order.
    [[nodiscard]]
    auto is_unpublished() -> bool;

    /// A reference to this process's thread with |koid|, or an empty one.
    /// Taken under the process lock with try_adopt(), since a listed thread
    /// may already have lost its last reference; nothing is dropped under it.
    [[nodiscard]]
    auto lookup_thread(KoId koid) -> ustl::Arc<ThreadDispatcher>;

    /// The job's koid: a process is bound to the job it was created in for
    /// its whole life (m_job, set by the constructor and never changed).
    [[nodiscard]]
    auto related_koid() const -> KoId override;

    /// The job this process was created in, and belongs to for its whole
    /// life; see related_koid().
    [[nodiscard]]
    auto job() -> JobDispatcher &;

    /// Whether all of [addr, addr + len) lies inside this process's vDSO: the
    /// question the syscall path asks of every trap, to refuse -- and kill the
    /// process for -- one whose `syscall` instruction is anywhere else. That is
    /// what makes "syscalls only through the vDSO" a property the kernel
    /// enforces rather than a convention programs are asked to follow. The
    /// range is the address space's (mm::VmAspace::vdso_contains()), put there
    /// by the mapping of the vDSO's VMO.
    [[nodiscard]]
    FORCE_INLINE
    auto vdso_contains(VirtAddr addr, usize len) const -> bool {
        return m_aspace->vdso_contains(addr, len);
    }

    /// Lets every thread of this process use the I/O ports [base, base +
    /// count) from ring 3, on every CPU it runs on, once this returns: what
    /// ioports_request does after checking its resource. The ports are the
    /// address space's to keep (mm::ArchVmAspace::open_ioports()), since its
    /// switch is what changes them on a CPU, and a process has one.
    [[nodiscard]]
    auto open_ioports(u32 base, u32 count) -> Status;

    /// Refuses those ports to this process's threads again, as
    /// ioports_release does.
    [[nodiscard]]
    auto close_ioports(u32 base, u32 count) -> Status;

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

    // The job keeps its list through m_job_hook.
    friend class JobDispatcher;

    static constexpr auto MAGIC = gktl::magic("ProcessDispatcher");

    ProcessDispatcher(ustl::Arc<JobDispatcher> job, char const *name,
                      ustl::Arc<mm::VmAspace> aspace, ustl::Arc<HandleTable> handle_table,
                      ustl::Arc<HandleAccount> handle_account);
    ~ProcessDispatcher() override;

    /// The work of dying that must not run under the process lock; see the
    /// definition.
    auto finish_dead_transition() -> void;

    Self *m_parent;

    /// Under the same lock as m_thread_list; see attach_thread().
    State m_state TA_GUARDED(get_lock());

    /// The code the process ends with; see kill() and exit(). Read by
    /// get_info().
    i64 m_retcode TA_GUARDED(get_lock());

    /// When the process went Running, in nanoseconds on the monotonic clock;
    /// zero until then. Written once, by attach_thread() with the first
    /// thread, and read by get_info().
    Instant m_start_time TA_GUARDED(get_lock());

    ktl::Name<MAX_NAME_LEN> m_name;

    /// The job this process belongs to, held so the job outlives it.
    ///
    /// One-directional for now: the job does not keep a list of its processes.
    /// Such a list is what makes a job killable and enumerable as a unit, and
    /// neither operation exists here yet -- adding the back-pointer before
    /// either does would be a list nothing ever reads.
    ustl::Arc<JobDispatcher> m_job;

    /// Unique address space per each process.
    ///
    /// Owning, and it has to be: the address space is created by create() and
    /// referenced from nowhere else, so a raw pointer here would leave the
    /// only Arc on create()'s stack and free the aspace on return.
    ustl::Arc<mm::VmAspace> m_aspace;

    /// Held through an Arc rather than by value so that two processes can
    /// share one table, as create_shared() is to give them once it is
    /// defined; the base class HandleTable already carries the count for it.
    ustl::Arc<HandleTable> m_handle_table;

    /// See handle_account(). Shared, not owned: the handles this process
    /// made keep it as long as they live.
    ustl::Arc<HandleAccount> m_handle_account;

    /// See exceptionate(). Shut down first as the process dies
    /// (finish_dead_transition()).
    Exceptionate m_exceptionate;

    /// On its job's list of processes (JobDispatcher::add_process()) from the
    /// end of create() until it dies or is destroyed. Under the job's lock.
    ustl::collections::intrusive::ListMemberHook<> m_job_hook;

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

    /// Every thread of this process that has been started and has not yet
    /// exited. Under the same lock as m_state; see attach_thread() for why
    /// the two share one.
    ThreadDispatcher::List m_thread_list TA_GUARDED(get_lock());

    /// For all of process.
    ustl::collections::intrusive::ListMemberHook<> m_created_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_created_hook, CreatedListOption);
    USTL_DECLARE_LIST(Self, Createdlist, CreatedListOption);
    static Createdlist CREATED_PROCESS_LIST;

  public:
    /// The list a job keeps of its processes, through m_job_hook.
    USTL_DECLARE_HOOK_OPTION(Self, m_job_hook, JobListOption);
    USTL_DECLARE_LIST(Self, JobList, JobListOption);
};

} // namespace ours::object

#endif // OURS_OBJECT_PROCESS_DISPATCHER_HPP
