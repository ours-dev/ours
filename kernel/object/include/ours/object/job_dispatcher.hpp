/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_JOB_DISPATCHER_HPP
#define OURS_OBJECT_JOB_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/types.hpp>

#include <gktl/canary.hpp>
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

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("JobDispatcher");
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_JOB_DISPATCHER_HPP