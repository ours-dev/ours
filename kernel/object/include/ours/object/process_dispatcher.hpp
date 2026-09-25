/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PROCESS_DISPATCHER_HPP
#define OURS_OBJECT_PROCESS_DISPATCHER_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/task/thread.hpp>

#include <ktl/name.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
/// Rights a freshly created process handle carries.
/// Zircon ZX_DEFAULT_PROCESS_RIGHTS.
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

    static constexpr auto MAX_NAME_SIZE = 32;

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

    auto attach_thread(ThreadDispatcher &thread) -> Status;

    /// Ends this process with |retcode|: it starts dying, and every thread
    /// in it is told to die. Returns at once; the threads exit on their own
    /// way back to ring 3, and the process object goes when the last
    /// reference to it does.
    ///
    /// Only the first kill's retcode counts. A process that is already dying
    /// keeps the reason it is dying for.
    ///
    /// Zircon counterpart: ProcessDispatcher::Kill() with KillAllThreadsLocked().
    auto kill(i64 retcode) -> void;

    auto detach_thread(ThreadDispatcher &thread) -> Status;

    /// Records where this process's vDSO landed.
    ///
    /// The syscall path refuses -- and kills the process for -- a trap whose
    /// `syscall` instruction is not wholly inside this range (vdso_contains()
    /// over the SYSCALL_INSN_SIZE bytes before the return address), which is
    /// what makes "syscalls only through the vDSO" a property
    /// the kernel enforces rather than a convention programs are asked to
    /// follow. Set once, while the process is still being built and before any
    /// thread of it has run.
    ///
    /// Zircon counterpart: VmAspace::vdso_code_mapping_, consulted by
    /// VDso::ValidSyscallPC on the same path.
    auto set_vdso_code_range(VirtAddr base, usize size) -> void {
        DEBUG_ASSERT(m_state == State::Initial, "the vDSO range is fixed before the process runs");
        m_vdso_code_base = base;
        m_vdso_code_size = size;
    }

    /// Whether all of [addr, addr + len) lies inside this process's vDSO.
    ///
    /// A range and not a point, because what is asked about is an instruction
    /// -- the `syscall` a trap came from -- and all of its bytes have to be the
    /// vDSO's. A process with no vDSO answers false for every range, which is
    /// the safe direction: it can make no system call at all rather than any.
    ///
    /// Written as two comparisons that cannot wrap: |len| no larger than the
    /// vDSO, then |addr|'s offset into it no further than what leaves room for
    /// |len|. The subtraction wraps an |addr| below the base to a huge offset,
    /// which the second comparison refuses -- the same shape
    /// ours::mm::is_kernel_address() uses for one end.
    [[nodiscard]]
    FORCE_INLINE
    auto vdso_contains(VirtAddr addr, usize len) const -> bool {
        return len <= m_vdso_code_size && addr - m_vdso_code_base <= m_vdso_code_size - len;
    }

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
                      ustl::Arc<mm::VmAspace> aspace, ustl::Arc<HandleTable> handle_table);
    ~ProcessDispatcher() override;

    /// The work of dying that must not run under the process lock; see the
    /// definition.
    auto finish_dead_transition() -> void;

    Self *m_parent;

    /// Guarded by get_lock(), together with m_thread_list. The one read
    /// outside it is set_vdso_code_range()'s assertion, made while the
    /// process is still being built by one thread.
    State m_state;

    /// The code the process ends with; see kill(). Guarded by get_lock().
    /// Nothing reads it yet: process_exit and get_info (P1-B) will.
    i64 m_retcode;

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

    /// Held through an Arc rather than by value because Zircon's
    /// create_shared() gives two processes one table; the base class
    /// HandleTable already carries the count for it.
    ustl::Arc<HandleTable> m_handle_table;

    /// Where this process's vDSO is mapped. Zero until set_vdso_code_range(),
    /// and a zero size is what makes vdso_contains() refuse everything.
    VirtAddr m_vdso_code_base;
    usize m_vdso_code_size;

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
    /// exited. Guarded by get_lock(), together with m_state; see
    /// attach_thread() for why the two share one lock.
    ThreadDispatcher::List m_thread_list;

    /// For all of process.
    ustl::collections::intrusive::ListMemberHook<> m_created_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_created_hook, CreatedListOption);
    USTL_DECLARE_LIST(Self, Createdlist, CreatedListOption);
    static Createdlist CREATED_PROCESS_LIST;
};

} // namespace ours::object

#endif // OURS_OBJECT_PROCESS_DISPATCHER_HPP
