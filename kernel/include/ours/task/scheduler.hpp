/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHEDULER_HPP
#define OURS_TASK_SCHEDULER_HPP 1

#include <ours/task/sched/scheduler.hpp>

namespace ours::task {
using sched::UnparkControl;

using sched::DEFAULT_PRIORITY;
using sched::HIGHEST_PRIORITY;
using sched::IDLE_PRIORITY;
using sched::LOWEST_PRIORITY;

using sched::CpuAffinity;
using sched::Scheduler;

} // namespace ours::task

#endif // #ifndef OURS_TASK_SCHEDULER_HPP