/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_JOB_DISPATCHER_HPP
#define OURS_OBJECT_JOB_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/types.hpp>
#include <ustl/rc.hpp>

namespace ours::object {
using JobDispatcherBase = SoloDispatcher<JobDispatcher, Rights::None>;

class JobDispatcher: public JobDispatcherBase {
    using Self = JobDispatcher;

  public:
    static auto root_job() -> ustl::Arc<Self> &;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_JOB_DISPATCHER_HPP