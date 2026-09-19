/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_RUNTIME_HPP
#define OURS_TASK_SCHED_RUNTIME_HPP 1

#include <ours/sync/spin_lock.hpp>
#include <ours/task/sched/fair.hpp>
#include <ours/task/sched/rr_run_queue.hpp>
#include <ours/task/sched/sched_clock.hpp>
#include <ours/task/sched/sched_entity.hpp>
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

    /// This CPU's run queue for |Disci|.
    ///
    /// The queues live here, on the object a discipline is already handed,
    /// rather than on Scheduler. A discipline used to reach its own by
    /// recovering the whole Scheduler from this one --
    /// `Scheduler::of(runtime).rr_run_queue()`, a container_of round trip to
    /// fetch something that could have been in reach -- and that route is
    /// what let it reach anything else on the Scheduler too.
    ///
    /// Selected by the `Discipline` the caller is, which is the framework's
    /// own key: it is the template argument a discipline already supplies to
    /// `Schedule<Derived, Discipline>` and already registers under, so the
    /// mapping lives here rather than in a typedef each discipline writes for
    /// itself. Linux nests the same way -- `struct rq` holds `cfs_rq`,
    /// `rt_rq` and `dl_rq` inline and hands the whole `rq` to a `sched_class`.
    template <Discipline Disci>
    FORCE_INLINE
    auto queue() -> auto & {
        if constexpr (Disci == Discipline::Normal) {
            return m_fair_queue;
        } else {
            static_assert(Disci != Disci, "no run queue is registered for this discipline");
        }
    }

    /// RoundRobin's FIFO, which `queue<>()` cannot reach.
    ///
    /// RoundRobin is Fair's stand-in during bring-up and registers under
    /// `Discipline::Normal`, the same slot Fair claims, so the enum cannot
    /// tell the two apart -- see the assertion in `Scheduler::init()` that
    /// refuses them both at once. Until `Discipline` distinguishes them, this
    /// one is reached by name.
    FORCE_INLINE
    auto rr_run_queue() -> RrRunQueue & {
        return m_rr_queue;
    }

    FORCE_INLINE
    auto clock() -> SchedTime {
        return clock_source.clock();
    }

    /// How long the running thread has been running since it was last charged,
    /// advancing the mark to now in the same step. Zero when the clock has not
    /// moved, and the mark does not move either in that case.
    ///
    /// Read and write were always one operation, and both disciplines spelled
    /// it out by hand -- the same three lines in RoundRobin::charge() and in
    /// Fair::update_thread() -- so the two copies had already drifted apart:
    /// RR returned early at `elapsed <= 0`, Fair at `elapsed < 0`, meaning Fair
    /// charged a zero-length interval that RR did not. Linux factors exactly
    /// these lines out of every class's update_curr() into update_curr_common()
    /// in core.c and settles the boundary there, the way it is settled here:
    /// `if (unlikely((s64)delta_exec <= 0)) return;`. The mark never moves
    /// backwards, and a zero-length interval is not an event.
    ///
    /// What to do with the answer stays with the discipline -- RR adds it to
    /// the thread's runtime, Fair also weights it into a virtual runtime --
    /// which is why this returns the duration instead of applying it.
    [[nodiscard]]
    FORCE_INLINE
    auto charge_current() -> SchedDuration {
        auto const now = clock();
        auto const elapsed = now - curr_started_time;
        if (elapsed <= SchedDuration{0}) [[unlikely]] {
            return SchedDuration{0};
        }

        curr_started_time = now;
        return elapsed;
    }

    /// Start the running thread's slice at now, charging nobody for what came
    /// before.
    ///
    /// Distinct from charge_current() on purpose. It is called when a *new*
    /// thread takes the CPU, where the interval since the mark belongs to the
    /// thread being replaced and has already been charged to it; folding the
    /// two together would charge that interval twice, once to each.
    FORCE_INLINE
    auto restart_current_slice() -> void {
        curr_started_time = clock();
    }

    /// Ask that |next| be the thread chosen at the next reschedule.
    ///
    /// The directed-yield channel. It has to be a slot on the run queue rather
    /// than a return value because the nomination and the choice happen in two
    /// different critical sections -- Current::yield_to_thread() takes the
    /// lock, records this and drops it; reschedule_common() takes it again and
    /// reads it. Linux's counterpart is the `rq->...` hand-off inside
    /// yield_to_task_fair().
    ///
    /// Named rather than assigned so that the one legitimate write is the only
    /// write: evaluate_next_thread() already *returns* the next thread, and a
    /// discipline storing into the slot as well gave the same answer two ways
    /// out.
    FORCE_INLINE
    auto nominate_next_thread(Thread *next) -> void {
        next_thread = next;
    }

    /// Reprogram the preemption timer to fire |delta| from now.
    ///
    /// Programs the timer of the CPU *executing* this call, not of
    /// which_cpu(): Driver::reset_preempt() reaches TickDevice::Current::get(),
    /// which has no CpuNum parameter to give it. Every caller today is on the
    /// local reschedule path, so the two coincide -- but a Runtime handed to a
    /// discipline is not always the running CPU's, and if this is ever reached
    /// from a remote one the timer lands on the wrong CPU with no diagnostic.
    /// The assertion below says so where it can; it is compiled out at
    /// KERNEL_DEBUG_LEVEL=0, so it is a tripwire for the debug image, not a
    /// guarantee.
    FORCE_INLINE
    auto arm_preempt_timer(SchedDuration delta) -> void {
        DEBUG_ASSERT(which_cpu == CpuNum(Cpu::id()));
        clock_source.reset_preempt(delta);
    }

    CpuNum which_cpu;
    sync::SpinLock rqlock;

    SchedClock clock_source;

    Weight total_weight;
    usize num_queued_threads;
    Thread *prev_thread;
    Thread *curr_thread;
    Thread *next_thread;
    SchedTime curr_started_time;
    SchedTime curr_preempt_time;

    sync::SpinLock migrating_threads_lock;
    Thread::MigratingList migrating_threads;

    // Physical time.
    RunQueue m_fair_queue;
    RrRunQueue m_rr_queue;

    SchedTime last_updated_time;
    SchedDuration total_expected_runtime;
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RUNTIME_HPP