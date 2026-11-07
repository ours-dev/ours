/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_CONTEXT_HPP
#define OURS_TASK_SCHED_CONTEXT_HPP 1

#include <ours/task/sched/scheduler_state.hpp>

namespace ours::task::sched {
/// Compatibility name for the per-thread scheduler state.
using Context = SchedulerState;
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_CONTEXT_HPP
