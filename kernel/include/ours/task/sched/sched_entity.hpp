/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHED_ENTITY_HPP
#define OURS_TASK_SCHED_SCHED_ENTITY_HPP 1

#include <ours/task/sched/scheduler_state.hpp>

namespace ours::task::sched {
/// The name still used by the Fair discipline.
using SchedEntity = SchedulerState;
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHED_ENTITY_HPP
