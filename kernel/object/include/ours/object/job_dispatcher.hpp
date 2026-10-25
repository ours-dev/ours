/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_JOB_DISPATCHER_HPP
#define OURS_OBJECT_JOB_DISPATCHER_HPP 1

#include <ours/exception.hpp>
#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/exceptionate.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/types.hpp>

#include <gktl/canary.hpp>
#include <ktl/vec.hpp>
#include <ustl/rc.hpp>

namespace ours::object {
/// Rights a freshly created job handle carries. It was Rights::None, which
/// would have made every job handle useless the moment a system call checked
/// it.
constexpr auto RIGHTS_DEFAULT_JOB = Rights::Basic | Rights::Io | Rights::Property | Rights::Policy |
                                    Rights::Enumerate | Rights::Destroy | Rights::Signal |
                                    Rights::ManageJob | Rights::ManageProcess |
                                    Rights::ManageThread;

using JobDispatcherBase = SoloDispatcher<JobDispatcher, RIGHTS_DEFAULT_JOB>;

class JobDispatcher: public JobDispatcherBase {
    using Self = JobDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Job);

    /// The job every process created during bring-up belongs to.
    ///
    /// Built once from an init hook (job_dispatcher.cpp) rather than handed
    /// out lazily, so the reference is either valid or the kernel has already
    /// panicked -- there is no state in which a process gets built without a
    /// job.
    static auto root_job() -> ustl::Arc<Self> &;

    /// Puts |process| on this job's list, which is what makes it listed
    /// (get_process_koids()) and reachable by koid (lookup_process()). Once,
    /// at the end of ProcessDispatcher::create(), when the process is whole.
    auto add_process(ProcessDispatcher &process) -> void;

    /// Takes |process| off the list if it is on it: when it dies
    /// (ProcessDispatcher::finish_dead_transition(), outside the process
    /// lock) and, for one that never died, when it is destroyed. A process
    /// leaves the list before its memory goes, and leaving takes this job's
    /// lock, so a listed process is always safe to read under that lock.
    auto remove_process(ProcessDispatcher &process) -> void;

    /// The koids of the processes listed, at most |max| into |koids|, and how
    /// many there are into |avail|. A process its creator has not handed out
    /// yet (ProcessDispatcher::is_unpublished()) is not counted. Counted
    /// under the job's lock, allocated without it, then filled; the second
    /// count is |avail|. NoMemory if the koids cannot be had.
    [[nodiscard]]
    auto get_process_koids(usize max, ktl::Vec<KoId> *koids, usize *avail) -> Status;

    /// A reference to the listed process with |koid|, or an empty one: taken
    /// with try_adopt() under the job's lock, since a listed process may
    /// already have lost its last reference, and never dropped under it, since
    /// dropping the last one runs the destructor, which takes this lock.
    [[nodiscard]]
    auto lookup_process(KoId koid) -> ustl::Arc<ProcessDispatcher>;

    /// Binds |channel|, the kernel's end of a fresh channel, as one more of
    /// this job's debugger channels (task_create_exception_channel with
    /// Debugger). |thread_rights| and |process_rights| are what a debugger
    /// gets on the handles it asks an exception for, as for
    /// Exceptionate::set_channel(). AlreadyBound while
    /// MAX_JOB_DEBUGGER_CHANNELS are bound with their readers still there;
    /// one whose reader has gone no longer counts and is dropped here.
    /// NoMemory if its exceptionate cannot be made. On any refusal |channel|
    /// is dropped, and the other end sees PeerClosed.
    [[nodiscard]]
    auto bind_debugger(KernelHandle<ChannelDispatcher> channel, Rights thread_rights,
                       Rights process_rights) -> Status;

    /// What for_each_debugger() calls for each debugger channel: true to go
    /// on to the next.
    using DebuggerVisitor = bool (*)(void *context, Exceptionate &debugger);

    /// Calls |visit| with |context| on each of this job's debugger channels,
    /// in the order they were bound, until it answers false. The channels
    /// are copied out under the job's lock and visited without it: a visit
    /// sends an exception and waits for the debugger to answer it, for as
    /// long as the debugger takes.
    auto for_each_debugger(void *context, DebuggerVisitor visit) -> void;

  private:
    /// One debugger channel. Shared, because a visit holds it without the
    /// job's lock while a bind may drop it from the job.
    class DebugExceptionate final
        : public Exceptionate,
          public ustl::RefCounted<DebugExceptionate, ustl::sync::ThreadSafePolicy> {};

    /// Drops the debugger channels whose readers have gone, keeping the
    /// others in their order, and answers how many are left. The dropped
    /// ones are moved into |gone|, for the caller to release after the lock:
    /// the last reference to one closes its channel, which must not happen
    /// under a task's lock (Exceptionate::shutdown()).
    TA_REQUIRES(get_lock())
    auto prune_debuggers_locked(ustl::Arc<DebugExceptionate> (&gone)[MAX_JOB_DEBUGGER_CHANNELS])
        -> usize;

    ProcessDispatcher::JobList m_processes TA_GUARDED(get_lock());

    /// The debugger channels, in the order they were bound: the first
    /// m_debugger_count entries, and nothing after them. A fixed array
    /// rather than a list: there are at most MAX_JOB_DEBUGGER_CHANNELS, and
    /// a visit copies them out under the lock into one of the same size.
    ustl::Arc<DebugExceptionate> m_debuggers[MAX_JOB_DEBUGGER_CHANNELS] TA_GUARDED(get_lock());
    usize m_debugger_count TA_GUARDED(get_lock()) = 0;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_JOB_DISPATCHER_HPP