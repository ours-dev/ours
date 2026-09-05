/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP
#define OURS_TASK_SCHED_SCHEDULER_INLINE_HPP 1

#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>

namespace ours::task::sched {
FORCE_INLINE
inline auto Scheduler::put_prev_thread(Thread *prev) -> void {
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

/// No precondition on m_runtime.curr_thread here. This used to assert that it
/// was null, describing a hand-over protocol -- put_prev_thread() clears it,
/// set_next_thread() finds it empty -- that nothing implements: curr_thread
/// has exactly one writer, switch_context(), which runs *after* this, and
/// put_prev_thread() leaves it alone. The assertion was unreachable only
/// while curr_thread was never seeded at all (see init_first_thread()); with
/// a real current thread it failed on the boot's first switch. Zircon writes
/// active_thread_ at the switch as well, not at the pick.
FORCE_INLINE
inline auto Scheduler::set_next_thread(Thread *next) -> void {
    if (next == m_idle_thread) {
        set_state(State::Idling, true);
    }
    next->set_state_running();

    auto &ctx = next->scheduling_context();
    ctx.schedule()->set_next_thread(m_runtime, next);
}
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP