/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_JOB_DISPATCHER_HPP
#define OURS_OBJECT_JOB_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
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

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("JobDispatcher");

    ProcessDispatcher::JobList m_processes TA_GUARDED(get_lock());
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_JOB_DISPATCHER_HPP