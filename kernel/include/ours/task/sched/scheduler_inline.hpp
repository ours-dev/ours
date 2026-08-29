/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP
#define OURS_TASK_SCHED_SCHEDULER_INLINE_HPP 1

#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>

namespace ours::task::sched {
FORCE_INLINE inline auto Scheduler::put_prev_thread(Thread *prev) -> void {
    DEBUG_ASSERT(prev == m_runtime.curr_thread);

    if (prev == m_idle_thread) {
        set_state(State::Idling, false);
    }
    if (prev->is_state_running()) {
        prev->set_state_ready();
    }

    auto &ctx = prev->scheduling_context();
    ctx.schedule()->put_prev_thread(m_runtime, prev);
}

FORCE_INLINE inline auto Scheduler::set_next_thread(Thread *next) -> void {
    DEBUG_ASSERT(0 == m_runtime.curr_thread);

    if (next == m_idle_thread) {
        set_state(State::Idling, true);
    }
    next->set_state_running();

    auto &ctx = next->scheduling_context();
    ctx.schedule()->set_next_thread(m_runtime, next);
}
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP