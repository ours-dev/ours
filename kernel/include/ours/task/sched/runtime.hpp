/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_RUNTIME_HPP
#define OURS_TASK_SCHED_RUNTIME_HPP 1

#include <ours/sync/spin_lock.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/sched/driver.hpp>
#include <ours/task/sched/types.hpp>
#include <ours/task/thread.hpp>

#include <gktl/canary.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours::task::sched {
/// Data required for executing the scheduler. The class is passed around to
/// a function that will perform the scheduling work and acts as a capability
/// token.
struct Runtime {
    /// Records that this Runtime's CPU should preempt what it is running.
    ///
    /// |which_cpu|, not add_pending()'s default. The default is the mask of
    /// the CPU *executing* the call, and a Runtime is not always the running
    /// CPU's: Scheduler::do_unpark_thread() reaches into the target CPU's
    /// Runtime from the waker, and every ISchedule entry point is handed a
    /// Runtime that may be remote for the same reason. Left at the default, a
    /// cross-CPU wakeup marked the waker as needing to preempt and told the
    /// target nothing.
    ///
    /// This is the one place that rule is written down; callers say
    /// `mark_preempt_pending()` and the local case falls out of it.
    auto mark_preempt_pending() -> void {
        curr_thread->preemption_state().add_pending(CpuMask::of(which_cpu));
    }

    FORCE_INLINE
    auto clock() -> SchedTime {
        return driver.clock();
    }

    /// Calculate the scheduling period
    FORCE_INLINE
    auto scheduling_period() const -> SchedDuration {
        return minimal_granularity * static_cast<i64>(scheduling_period_grans);
    }

    FORCE_INLINE
    auto min_scheduling_period() const -> SchedDuration {
        return minimal_granularity * static_cast<i64>(minimal_period_grans);
    }

    constexpr static auto DEFAULT_MIN_SCHED_GRANULARITY = Milliseconds(1);

    /// Each process then runs for a “timeslice” proportional to its weight divided by the total weight of
    /// all runnable threads. To calculate the actual timeslice, CFS sets a target for its approximation of
    /// the “infinitely small” scheduling duration in perfect multitasking. This target is called the targeted
    /// latency. Smaller targets yield better interactivity and a closer approximation to perfect multitasking,
    /// at the expense of higher switching costs and thus worse overall throughput.
    constexpr static auto DEFAULT_MIN_TARGETED_LATENCY = Milliseconds(8);

    CpuNum which_cpu;
    sync::SpinLock rqlock;

    Driver driver;

    Weight total_weight;
    usize num_queued_threads;
    Thread *prev_thread;
    Thread *curr_thread;
    Thread *next_thread;
    SchedTime curr_started_time;
    SchedTime curr_preempt_time;

    /// Scheduling period in which every runnable task executes once in units of
    /// minimal granularity.
    usize scheduling_period_grans = DEFAULT_MIN_TARGETED_LATENCY / DEFAULT_MIN_SCHED_GRANULARITY;

    /// Scheduling period in which every runnable task executes once in units of
    /// minimal granularity.
    usize minimal_period_grans = DEFAULT_MIN_TARGETED_LATENCY / DEFAULT_MIN_SCHED_GRANULARITY;

    /// The smallest timeslice a thread is allocated in a single round.
    SchedDuration minimal_granularity = to_sched_duration(DEFAULT_MIN_TARGETED_LATENCY);

    sync::SpinLock migrating_threads_lock;
    Thread::MigratingList migrating_threads;

    // Physical time.
    SchedTime last_updated_time;
    SchedDuration total_expected_runtime;
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RUNTIME_HPP