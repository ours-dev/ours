/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_RUN_QUEUE_HPP
#define OURS_TASK_SCHED_RUN_QUEUE_HPP 1

#include <ours/sync/spin_lock.hpp>
#include <ours/task/sched/sched_clock.hpp>

// Whichever discipline occupies Discipline::Normal brings its own queue type
// with it, so only that one's header is pulled in.
#if SCHED_NORMAL_DISCIPLINE_FAIR
#include <ours/task/sched/fair.hpp>
#else
#include <ours/task/sched/rr_run_queue.hpp>
#endif
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/sched/types.hpp>
#include <ours/task/thread.hpp>

#include <gktl/canary.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours::task::sched {
/// The run queue of whichever discipline occupies `Discipline::Normal`.
///
/// RoundRobin and Fair are mutually exclusive -- RoundRobin is the stand-in
/// that keeps the kernel running while the rest of it is brought up, and Fair
/// is the one that replaces it -- so the slot has one queue, of whichever
/// type the selected discipline uses. This used to be two members side by
/// side with a named accessor for the stand-in, which made the transitional
/// discipline look like a permanent second citizen; it is not, it is the one
/// that is here now.
///
/// The choice is `sched_normal_discipline` in //kernel/kernel_params.gni, the
/// same shape as MemMap's two mutually exclusive implementations.
///
/// The Fair branch names `FairRunQueue`, which Fair does not define yet: its
/// class is still called `RunQueue`, the name this per-CPU object has now
/// taken. Renaming it is part of bringing Fair back -- see the adaptation
/// list in docs/design/260918 -- and until then the Fair branch does not
/// build, which is the same thing the rest of that list already says.
#if SCHED_NORMAL_DISCIPLINE_FAIR
using NormalRunQueue = FairRunQueue;
#else
using NormalRunQueue = RrRunQueue;
#endif

/// Data required for executing the scheduler. The class is passed around to
/// a function that will perform the scheduling work and acts as a capability
/// token.
/// One CPU's run queue: the lock, the CPU's identity, the scheduler clock,
/// the discipline's queue, and the bookkeeping a scheduling decision is made
/// from.
///
/// It is a class with private fields and exactly one friend, and the public
/// surface below is the measured need -- nothing invented, nothing measured
/// left out. It used to be a struct of fourteen public fields handed by
/// reference to sixteen virtuals, so a discipline could write
/// `rq.clock_source.update_clock()` (defeating the caller's SkipClock
/// decision), `rq.total_weight -= w` (corrupting the generic Scheduler's
/// accounting), or reach the whole Scheduler through `Scheduler::of()`.
///
/// Deliberately NOT a friend of any discipline. C++ friendship is class-wide:
/// `friend class Fair` would hand Fair every private member and make every
/// claim above false.
class RunQueue {
    friend class Scheduler;

  public:
    /// Records that this RunQueue's CPU should preempt what it is running.
    ///
    /// |m_which_cpu|, not add_pending()'s default. The default is the mask of
    /// the CPU *executing* the call, and a RunQueue is not always the running
    /// CPU's: Scheduler::do_unpark_thread() reaches into the target CPU's
    /// RunQueue from the waker, and every entry point of a discipline is handed a
    /// RunQueue that may be remote for the same reason. Left at the default, a
    /// cross-CPU wakeup marked the waker as needing to preempt and told the
    /// target nothing.
    ///
    /// This is the one place that rule is written down; callers say
    /// `mark_preempt_pending()` and the local case falls out of it.
    auto mark_preempt_pending() -> void {
        m_curr_thread->preemption_state().add_pending(CpuMask::of(m_which_cpu));
    }

    /// This CPU's run queue for |Disci|.
    ///
    /// The queues live here, on the object a discipline is already handed,
    /// rather than on Scheduler. A discipline used to reach its own by
    /// recovering the whole Scheduler from this one --
    /// `Scheduler::of(rq).rr_run_queue()`, a container_of round trip to
    /// fetch something that could have been in reach -- and that route is
    /// what let it reach anything else on the Scheduler too.
    ///
    /// Selected by the `Discipline` the caller is, which is the framework's
    /// own key: it is the template argument a discipline already supplies to
    /// `Schedule<Derived, Discipline>` and already registers under, so the
    /// mapping lives here rather than in a typedef each discipline writes for
    /// itself.
    /// The run-queue lock, and the one capability every annotation in this
    /// subsystem names.
    ///
    /// It has to stay publicly nameable even though the member is private:
    /// clang resolves a thread-safety attribute's argument *with access
    /// control on*, so `TA_REQUIRES(rq.m_rqlock)` in schedule.hpp would
    /// be a hard error rather than a warning. TA_RETURN_CAPABILITY makes this
    /// accessor and the member one capability to the analysis, the same
    /// construction `Scheduler::get_lock()` already uses.
    ///
    /// This is also the honest limit of the seal: a discipline can still call
    /// `rq.lock().acquire()`. No design that spells `TA_REQUIRES(param.x)`
    /// can close that, because the capability must be nameable to be named.
    TA_RETURN_CAPABILITY(m_rqlock) FORCE_INLINE auto lock() -> sync::SpinLock & {
        return m_rqlock;
    }

    /// The CPU this run queue is -- never `Cpu::id()`.
    ///
    /// A RunQueue handed to a discipline is not always the running CPU's:
    /// `Scheduler::do_unpark_thread()` runs on the waker and mutates the
    /// target's queue. Reading the executing CPU instead is the mistake this
    /// subsystem has already made once.
    [[nodiscard]]
    FORCE_INLINE
    auto which_cpu() const -> CpuNum {
        return m_which_cpu;
    }

    /// The thread running on `which_cpu()` -- not `Thread::Current::get()`,
    /// for the same reason. Read-only here: the writer is the context switch,
    /// which is Scheduler's.
    [[nodiscard]]
    FORCE_INLINE
    auto current_thread() const -> Thread * {
        return m_curr_thread;
    }

    template <Discipline Disci>
    FORCE_INLINE
    auto queue() -> auto & {
        if constexpr (Disci == Discipline::Normal) {
            return m_normal_queue;
        } else {
            static_assert(Disci != Disci, "no run queue is registered for this discipline");
        }
    }

    FORCE_INLINE
    auto clock() -> SchedTime {
        return m_clock_source.clock();
    }

    /// How long the running thread has been running since it was last charged,
    /// advancing the mark to now in the same step. Zero when the clock has not
    /// moved, and the mark does not move either in that case.
    ///
    /// Read and write were always one operation, and both disciplines spelled
    /// it out by hand -- the same three lines in RoundRobin::charge() and in
    /// Fair::update_thread() -- so the two copies had already drifted apart:
    /// RR returned early at `elapsed <= 0`, Fair at `elapsed < 0`, meaning Fair
    /// charged a zero-length interval that RR did not. Factored out here, the
    /// boundary is settled once, for every discipline: `elapsed <= 0` charges
    /// nothing. The mark never moves backwards, and a zero-length interval is
    /// not an event.
    ///
    /// What to do with the answer stays with the discipline -- RR adds it to
    /// the thread's rq, Fair also weights it into a virtual rq --
    /// which is why this returns the duration instead of applying it.
    [[nodiscard]]
    FORCE_INLINE
    auto charge_current() -> SchedDuration {
        auto const now = clock();
        auto const elapsed = now - m_curr_started_time;
        if (elapsed <= SchedDuration{0}) [[unlikely]] {
            return SchedDuration{0};
        }

        m_curr_started_time = now;
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
        m_curr_started_time = clock();
    }

    /// Ask that |next| be the thread chosen at the next reschedule.
    ///
    /// The directed-yield channel. It has to be a slot on the run queue rather
    /// than a return value because the nomination and the choice happen in two
    /// different critical sections -- Current::yield_to_thread() takes the
    /// lock, records this and drops it; reschedule_common() takes it again and
    /// reads it.
    ///
    /// Named rather than assigned so that the one legitimate write is the only
    /// write: evaluate_next_thread() already *returns* the next thread, and a
    /// discipline storing into the slot as well gave the same answer two ways
    /// out.
    FORCE_INLINE
    auto nominate_next_thread(Thread *next) -> void {
        m_next_thread = next;
    }

    /// Set this CPU's preemption deadline |delta| past the scheduler's last
    /// reading of the clock, which a pick has just taken: the end of the
    /// running thread's slice, kept in the CPU's timer queue
    /// (SchedClock::set_preemption_deadline()).
    ///
    /// Sets the deadline of the CPU *executing* this call, not of
    /// which_cpu(): SchedClock::set_preemption_deadline() reaches TimerQueue::get(),
    /// which has no CpuNum parameter to give it. Every caller today is on the
    /// local reschedule path, so the two coincide -- but a RunQueue handed to a
    /// discipline is not always the running CPU's, and if this is ever reached
    /// from a remote one the timer lands on the wrong CPU with no diagnostic.
    /// The assertion below says so where it can; it is compiled out at
    /// KERNEL_DEBUG_LEVEL=0, so it is a tripwire for the debug image, not a
    /// guarantee.
    FORCE_INLINE
    auto set_preemption_deadline(SchedDuration delta) -> void {
        DEBUG_ASSERT(m_which_cpu == CpuNum(Cpu::id()));
        m_clock_source.set_preemption_deadline(delta);
    }

    /// No preemption deadline: this CPU runs its idle thread, which has no
    /// slice, so only its timers wake it. On this CPU, as set_preemption_deadline().
    FORCE_INLINE
    auto clear_preemption_deadline() -> void {
        DEBUG_ASSERT(m_which_cpu == CpuNum(Cpu::id()));
        m_clock_source.clear_preemption_deadline();
    }

  private:
    /// Calls |visit| on every thread queued on this CPU, over every
    /// discipline's queue, until it answers false: the scheduler's view of
    /// what waits here, for a question no discipline orders by -- which
    /// threads may run on another CPU, as this one goes offline
    /// (Scheduler::Current::deactivate()). The running thread is not queued
    /// and is not visited. Each discipline's queue type supplies its part of
    /// the walk (for_each()).
    ///
    /// Private, for the scheduler, the one friend: a discipline is handed
    /// this RunQueue to see its own queue, not to walk the others'. Under the
    /// lock, and |visit| must not change the queues: a thread it picks is
    /// moved once the walk is over.
    template <typename Visit>
    auto for_each_thread(Visit &&visit) -> void {
        m_normal_queue.for_each(
            [&visit](SchedEntity &ctx) -> bool { return visit(*Thread::of(&ctx)); });
    }

    CpuNum m_which_cpu;
    /// Held with interrupts off by every holder: a timer's handler takes it
    /// from interrupt context to wake a thread (Scheduler::unpark_thread()),
    /// and a holder in thread context that the interrupt landed on would
    /// leave it spinning on its own CPU's lock.
    sync::SpinLock m_rqlock;

    SchedClock m_clock_source;

    Weight m_total_weight;
    usize m_num_queued_threads;
    Thread *m_prev_thread;
    Thread *m_curr_thread;
    Thread *m_next_thread;
    SchedTime m_curr_started_time;
    SchedTime m_curr_preempt_time;

    /// IrqSave like m_rqlock, whose run queue it feeds.
    sync::SpinLock m_migrating_threads_lock;
    Thread::MigratingList m_migrating_threads;

    // Physical time.
    NormalRunQueue m_normal_queue;

    SchedTime m_last_updated_time;
    SchedDuration m_total_expected_runtime;
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RUN_QUEUE_HPP