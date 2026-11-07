/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP
#define OURS_TASK_SCHED_SCHEDULER_INLINE_HPP 1

#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>

namespace ours::task::sched {
FORCE_INLINE
inline auto Scheduler::put_prev_thread(Thread *prev) -> void {
    DEBUG_ASSERT(prev == m_run_queue.m_curr_thread);

    if (prev == m_idle_thread) {
        set_state(State::Idling, false);
    }
    if (prev->is_state_running()) {
        prev->set_state_ready();
    }

    auto &ctx = prev->scheduling_entity();

    // It ran here, so this is where it last ran. Nothing wrote m_last_cpu
    // except init_first_thread(), which only ever runs for a CPU's idler and
    // the boot thread -- so for every ordinary thread select_target_cpu()'s
    // "prefer where it last ran" was reading a field frozen at construction.
    ctx.m_last_cpu = m_run_queue.m_which_cpu;

    schedule_of(ctx.discipline())->put_prev_thread(m_run_queue, prev);
}

/// No precondition on m_run_queue.m_curr_thread here. This used to assert that it
/// was null, describing a hand-over protocol -- put_prev_thread() clears it,
/// set_next_thread() finds it empty -- that nothing implements: curr_thread
/// has exactly one writer, switch_context(), which runs *after* this, and
/// put_prev_thread() leaves it alone. The assertion was unreachable only
/// while curr_thread was never seeded at all (see init_first_thread()); with
/// a real current thread it failed on the boot's first switch. The current
/// thread is written at the switch, not at the pick.
FORCE_INLINE
inline auto Scheduler::set_next_thread(Thread *next) -> void {
    if (next == m_idle_thread) {
        set_state(State::Idling, true);
    }
    next->set_state_running();

    auto &ctx = next->scheduling_entity();

    // The running thread belongs to this CPU. It is not enough to record this
    // at enqueue: a discipline takes the thread it is about to run *off* its
    // queue (RoundRobin::set_next_thread does), so "running here" is a state
    // enqueue_thread() never sees.
    ctx.m_curr_cpu = m_run_queue.m_which_cpu;

    schedule_of(ctx.discipline())->set_next_thread(m_run_queue, next);

    // The slice the discipline has just started ends at this CPU's preemption
    // deadline, so it is set on every pick, the running thread picked again
    // included, and nothing ticks to see whether the slice is over. The idle
    // thread has no slice: an idle CPU is woken by its timers alone.
    if (next == m_idle_thread) {
        m_run_queue.clear_preemption_deadline();
    } else {
        m_run_queue.set_preemption_deadline(ctx.remaining_slice());
    }
}
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULER_INLINE_HPP