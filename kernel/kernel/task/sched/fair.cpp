#include <ours/task/sched/config.hpp>
#include <ours/task/sched/fair.hpp>
#include <ours/task/sched/features.hpp>
#include <ours/task/sched/run_queue.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer_queue.hpp>

#include "ours/task/sched/profiles.hpp"
#include <logz4/log.hpp>
#include <mtl/ratio.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/mem/address_of.hpp>

namespace ours::task::sched {
class Observer {
    using Self = RunQueue;

  public:
    static auto on_insert() -> void;

    static auto on_erase() -> void;

    static auto on_rotate(SchedEntity &before, SchedEntity &after) -> void;

    static auto propagate(SchedEntity &object, SchedEntity &terminal) -> void;

    static auto copy(SchedEntity &from, SchedEntity &to) -> void;
};

// FairRunQueue itself -- all of Fair's per-CPU state -- is declared in
// fair.hpp and held directly by the per-CPU RunQueue.

auto FairRunQueue::term_of(SchedEntity &object) const -> WeightedTime {
    return weighted_offset(object.vruntime() - zero_vruntime_, object.weight());
}

auto FairRunQueue::dequeue_object(SchedEntity &object) -> void {
    (void)queue_.erase(queue_.iterator_to(object));

    weight_sum_ -= object.weight();
    total_vruntime_ -= term_of(object);
}

auto FairRunQueue::enqueue_object(SchedEntity &object) -> void {
    (void)queue_.insert(object);

    weight_sum_ += object.weight();
    total_vruntime_ += term_of(object);
}

auto FairRunQueue::sums() const -> Sums {
    auto terms = Sums{total_vruntime_, weight_sum_};
    if (current_ && current_->runqueue_state().is_active()) {
        terms.total += term_of(*current_);
        terms.weight += current_->weight();
    }
    return terms;
}

/// RelativeWeightedOwnedVirtualTime(i) = Weight(i) * (OwnedVirtualTime(i) - ZeroVirtualTime);
/// RelativeWeightedOwnedVirtualTimeSum = \Sum RelativeWeightedOwnedVirtualTime(i)
/// WeightedOwnedVirtualTimeSum = RelativeWeightedOwnedVirtualTimeSum + ZeroVirtualTime * TotalWeight
///
/// If ZeroVirtualTime will be plus Delta, then
///     RelativeWeightedOwnedVirtualTime(i) -= Weight(i) * Delta
/// ctx that
///     RelativeWeightedOwnedVirtualTimeSum -= Delta * TotalWeight
///
/// The sum here is the tree's alone, so it is moved by the tree's weight. The
/// running thread's term is not stored: sums() takes it from its vruntime at
/// the zero of the moment.
///
/// Nothing in the tree has to be searched for: it is ordered by deadline, so its
/// leftmost is not the thread with the smallest virtual time, and the weighted
/// mean is already in the two sums.
auto FairRunQueue::update_zero_vruntime(SchedDuration const delta) -> void {
    total_vruntime_ -= weighted_offset(delta, weight_sum_);
    zero_vruntime_ += delta;
}

auto FairRunQueue::recenter() -> void {
    auto const terms = sums();
    if (terms.weight == Weight{0}) {
        return;
    }

    update_zero_vruntime(weighted_mean_offset(terms.total, terms.weight));
}

auto FairRunQueue::avg_vruntime() -> SchedDuration {
    recenter();
    return zero_vruntime_;
}

/// First, `IdealServiceVirtualTime` is same to all task.
///
/// Lag(i) = IdealServiceTime - OwnedServiceTime(i)
///        = Weight(i) * (IdealServiceVirtualTime - OwnedServiceVirtualTime(i))
///
/// Out of \Sum Lag(i) = \Sum Weight(i) * (`IdealServiceVirtualTime` - OwnedServiceVirtualTime(i))
///                    = \Sum Weight(i) * (`IdealServiceVirtualTime` - OwnedServiceVirtualTime(i))
///                    = TotalWeight * `IdealServiceVirtualTime` - \Sum Weight(i) * OwnedServiceVirtualTime(i)
///                    = TotalWeight * `IdealServiceVirtualTime` - WeightedOwnedVirtualTimeSum
///                    = 0
/// ctx `IdealServiceVirtualTime` = WeightedOwnedVirtualTimeSum / TotalWeight
///                              = WeightedAverageOwnedVirtualTime
///
/// When the lag of a task is greater than or equal to zero, it is eligible,
/// otherwise ineligible.
///
/// The code tests it as `WeightedOwnedVirtualTimeSum >= Weight * (OwnedVirtualTime(i)
/// - ZeroVirtualTime)` with TotalWeight in the place of Weight, the running thread
/// counted in the sum and in the weight, without the division.
auto FairRunQueue::is_eligible(SchedEntity const &object) const -> bool {
    auto const terms = sums();
    return offset_is_eligible(terms.total, terms.weight, object.vruntime() - zero_vruntime_);
}

/// Stateless: implements the EEVDF algorithm only. All per-CPU state lives
/// in FairRunQueue (fair.hpp), held by the per-CPU RunQueue -- one shared Fair
/// instance serves every CPU, fetching the queue from the RunQueue it is handed.
class Fair final {
    using Self = Fair;

  public:
    auto enqueue_thread(RunQueue &, Thread *, EnqueueOptions) -> Status;
    auto dequeue_thread(RunQueue &, Thread *, DequeueOptions) -> Status;

    auto yield_thread(RunQueue &, Thread *) -> void;
    auto yield_to_thread(RunQueue &, Thread *, Thread *) -> Status;

    auto evaluate_next_thread(RunQueue &, Thread *) -> Thread *;
    auto put_prev_thread(RunQueue &, Thread *) -> void;
    auto set_next_thread(RunQueue &, Thread *) -> void;

    auto update_thread(RunQueue &, Thread *curr) -> void;

    auto on_thread_init(RunQueue &, Thread *) -> void;
    auto on_thread_dead(RunQueue &, Thread *) -> void;
    auto on_thread_unpark(RunQueue &, Thread *) -> void;
    auto on_thread_tick(RunQueue &, Thread *, bool queued) -> void;
    auto unpark_preempt(RunQueue &runtime, Thread *thread, Thread *curr, UnparkOptions) -> Status;

    /// Not the migrate_thread/select_target_cpu of the Schedule interface --
    /// Fair's versions need the per-CPU RunQueue that only this discipline's
    /// callers have on hand. Their signatures differ from the interface's, so
    /// the Scheduler does not call them and the interface's defaults answer
    /// instead; the two APIs are deliberately left unreconciled for now.
    auto migrate_thread(RunQueue &, Thread *, CpuNum) -> Status;
    auto select_target_cpu(RunQueue &, Thread *) -> CpuNum;

    auto override_thread_base_profile(RunQueue &, Thread *, BaseProfile const &) -> Status;

    [[nodiscard]]
    auto num_queued(RunQueue &runtime) const -> usize {
        return runtime.queue<Discipline::Normal>().num_queued();
    }

  private:
    auto try_to_delay_dequeue(RunQueue &, SchedEntity &, DequeueOptions) -> bool;

    auto adjust_vlag(RunQueue &runtime, SchedEntity &object) -> void;

    auto update_vlag(RunQueue &runtime, SchedEntity &object) -> void;

    auto set_next_object(RunQueue &runtime, SchedEntity &object) -> void;
};

auto Fair::adjust_vlag(RunQueue &runtime, SchedEntity &ctx) -> void {
    auto ideal_vruntime = runtime.queue<Discipline::Normal>().avg_vruntime();
    auto vlag = ctx.m_fair.vlag;

    auto &timeslice = ctx.m_timeslice;
    if (!timeslice.is_custom_timeslice()) {
        timeslice.m_base = static_cast<u32>(scheduling_period().raw());
    }

    auto &params = ctx.m_fair;
    if (vlag != SchedDuration{0}) {
        // Adjust the vlag.
    }

    // Compensates the |object|
    params.vruntime = ideal_vruntime - vlag;
    params.vdeadline = params.vruntime + to_virtual_time(timeslice.length(), ctx.weight());
}

FORCE_INLINE
auto Fair::update_vlag(RunQueue &runtime, SchedEntity &object) -> void {
    object.m_fair.vlag = runtime.queue<Discipline::Normal>().lag_of(object);
}

auto Fair::update_thread(RunQueue &runtime, Thread *curr) -> void {
    runtime.queue<Discipline::Normal>().verify();

    // No early return on a zero interval: charge_current() already declines to
    // move the mark then, adding zero is a no-op, and the deadline test below
    // still has to run -- a thread whose request is already spent must be
    // preempted on this tick whether or not the clock advanced since the last.
    auto const elapsed = runtime.charge_current();

    auto &ctx = curr->scheduling_entity();

    ctx.m_exec_runtime += elapsed;
    ctx.m_fair.vruntime += to_virtual_time(elapsed, ctx.weight());

    // Both sides are read on the virtual clock: vdeadline is some vruntime plus
    // one or more requests, so whatever offset vruntime started from cancels, and
    // the test holds once about one request of real time has been charged since
    // vdeadline was last set (about, because the weight scaling truncates).
    // exec_runtime() is the other clock, which does not start where vruntime does
    // and is not scaled by the weight.
    if (ctx.m_fair.vruntime >= ctx.virtual_deadline()) {
        // Note it has completely consumed it's request, ctx there is an occassion to preempt.
        runtime.mark_preempt_pending();

        ctx.m_fair.vdeadline =
            ctx.m_fair.vruntime + to_virtual_time(ctx.m_timeslice.length(), ctx.weight());

        // A thread that is only ever re-picked reaches no placement or lag, and its
        // offset from the zero would grow with its clock until it saturates.
        runtime.queue<Discipline::Normal>().recenter();
    }
}

auto Fair::try_to_delay_dequeue(RunQueue &runtime, SchedEntity &ctx, DequeueOptions options)
    -> bool {
    if (!!(DequeueOptions::Delayed & options)) {
        // Check the |ctx| is a delayed scheduling object.
        return false;
    }

    auto delayable = !!(DequeueOptions::Sleep & options);
    if (sched::has_feature(Feature::DelayDequeue) && delayable &&
        !runtime.queue<Discipline::Normal>().is_eligible(ctx)) {
        return true;
    }

    return false;
}

auto Fair::dequeue_thread(RunQueue &runtime, Thread *thread, DequeueOptions options) -> Status {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    auto const current_thread = runtime.current_thread();
    auto const is_current_thread = thread == current_thread;
    update_thread(runtime, current_thread);

    auto &ctx = thread->scheduling_entity();
    if constexpr (SCHED_DELAY_DEQUEUE) {
        if (try_to_delay_dequeue(runtime, ctx, options)) {
            return Status::Delayed;
        }
    }

    update_vlag(runtime, ctx);
    if (!is_current_thread) {
        rq.dequeue_object(ctx);
        rq.dec_queued();
    }
    ctx.runqueue_state().on_dequeue();

    return Status::Ok;
}

auto Fair::enqueue_thread(RunQueue &runtime, Thread *thread, EnqueueOptions options) -> Status {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    auto const current_thread = runtime.current_thread();
    auto const is_current_thread = thread == current_thread;

    auto &ctx = thread->scheduling_entity();

    // Re-enqueueing |current| typeically refers to dequeueing and then immediately
    // dequeueing the thread again. This mainly updates ctxme internal scheduling parameters,
    // but it has little to no effect on our vlag.
    if (is_current_thread) {
        adjust_vlag(runtime, ctx);
    }

    update_thread(runtime, current_thread);

    if (!is_current_thread) {
        adjust_vlag(runtime, ctx);
    }

    if (!is_current_thread) {
        rq.enqueue_object(ctx);
        rq.inc_queued();
    }
    ctx.runqueue_state().on_enqueue();

    return Status::Ok;
}

auto Fair::on_thread_tick(RunQueue &runtime, Thread *curr, bool queued) -> void {
    runtime.queue<Discipline::Normal>().verify();
    update_thread(runtime, curr);

    if (queued) {
        curr->preemption_state().add_pending();
    }
}

auto Fair::on_thread_init(RunQueue &runtime, Thread *thread) -> void {
    runtime.queue<Discipline::Normal>().verify();
    [[maybe_unused]]
    auto &ctx = thread->scheduling_entity();
}

auto Fair::on_thread_dead(RunQueue &runtime, Thread *thread) -> void {
    runtime.queue<Discipline::Normal>().verify();
    [[maybe_unused]]
    auto &ctx = thread->scheduling_entity();
}

/// The thread has just been made runnable again. By the time this runs
/// Scheduler::unpark() has already enqueued it, so placement -- the vlag
/// compensation in adjust_vlag() -- is done; what is left is the bookkeeping
/// that only the wakeup path knows.
auto Fair::on_thread_unpark(RunQueue &runtime, Thread *thread) -> void {
    runtime.queue<Discipline::Normal>().verify();

    // Where the wakeup landed. Nothing else records this, and it is what a
    // future select_target_cpu() needs to keep a woken thread near the cpu
    // that woke it rather than scattering it across the machine.
    thread->scheduling_entity().m_wake_cpu = runtime.which_cpu();
}

/// In order to provide latency guarantees for different request sizes
/// EEVDF selects the best runnable task from two criteria:
///  (1) the task must be eligible (must be owed service)
///
///  (2) from those tasks that meet (1), we select the one
///      with the earliest virtual deadline.
auto Fair::evaluate_next_thread(RunQueue &runtime, Thread *curr) -> Thread * {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();
    auto iter = rq.begin();
    for (auto const last = rq.end(); iter != last; ++iter) {
        if (rq.is_eligible(*iter)) {
            break;
        }
    }

    if (iter == rq.end()) {
        return 0;
    }

    return Thread::of(&*iter);
}

auto Fair::put_prev_thread(RunQueue &runtime, Thread *prev) -> void {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    auto &ctx = prev->scheduling_entity();

    // The thread |prev| was not been deactivated via Scheduler::deactivate_thread().
    // Therefore now it is still on our runqueue. It is charged while it is still
    // the running thread, so the mean the charge may recenter on counts it.
    auto const active = ctx.runqueue_state().is_active();
    if (active) {
        update_thread(runtime, prev);
    }
    rq.clear_current();
    if (active) {
        rq.enqueue_object(ctx);
    }
}

auto Fair::set_next_object(RunQueue &runtime, SchedEntity &ctx) -> void {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    // The next object may not be on queue, ctx it demands us to check as the following to
    // prevent double dequeue operations.
    if (ctx.runqueue_state().is_active()) {
        rq.dequeue_object(ctx);
    }
    rq.set_current(ctx);

    ctx.m_prev_exec_runtime = ctx.m_exec_runtime;
    ctx.m_fair.vdeadline =
        ctx.m_fair.vruntime + to_virtual_time(ctx.m_timeslice.length(), ctx.weight());
}

auto Fair::set_next_thread(RunQueue &runtime, Thread *next) -> void {
    runtime.queue<Discipline::Normal>().verify();

    auto &ctx = next->scheduling_entity();
    set_next_object(runtime, ctx);

    if constexpr (SCHED_NOHZ) {
        // When configured NOHZ, need we to manuaully reset the preemption time point.
        // It is possiablely that the next thread has been woken just now, ctx recalculate
        //
        auto const consumed_timeslice = ctx.m_exec_runtime - ctx.m_prev_exec_runtime;
        auto const remaining_timeslice = ctx.m_timeslice.length() - consumed_timeslice;
        if (remaining_timeslice < SchedDuration{0}) {
            return;
        }

        runtime.arm_preempt_timer(remaining_timeslice);
    }
}

/// |curr| voluntarily gives up the cpu.
///
/// Under EEVDF the pick is "the eligible object with the earliest virtual
/// deadline", so yielding is not a requeue -- it is giving up that earliest
/// deadline. Pushing the deadline out by one whole request is what lets some
/// other runnable thread win the next pick, while leaving eligibility (and
/// therefore long-run fairness) untouched.
auto Fair::yield_thread(RunQueue &runtime, Thread *curr) -> void {
    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    // Nothing else to yield to; charging time and moving the deadline would
    // only penalise |curr| for no one's benefit.
    if (!rq.num_queued()) {
        return;
    }

    // Charge what it has used so far, so the deadline is pushed out from an
    // up-to-date vruntime rather than a stale one.
    update_thread(runtime, curr);

    auto &ctx = curr->scheduling_entity();
    ctx.m_fair.vdeadline += to_virtual_time(ctx.m_timeslice.length(), ctx.weight());

    runtime.mark_preempt_pending();
}

auto Fair::yield_to_thread(RunQueue &runtime, Thread *curr, Thread *next) -> Status {
    runtime.queue<Discipline::Normal>().verify();

    if (!next->scheduling_entity().runqueue_state().is_active()) {
        // Received a inactive thread.
        return Status::BadState;
    }

    update_thread(runtime, curr);
    runtime.nominate_next_thread(next);
    return Status::Ok;
}

auto Fair::unpark_preempt(RunQueue &runtime, Thread *thread, Thread *curr, UnparkOptions)
    -> Status {
    return Status::Unimplemented;
}

/// Changes |thread|'s base profile.
///
/// A queued thread's position depends on its weight, so the weight cannot be
/// changed in place: the thread comes off the runqueue, the profile changes,
/// and it goes back on. That is the same dance Scheduler::Pi::common() does
/// around a priority-inheritance change, for the same reason.
///
/// TODO(SmallHuaZi): folding the new base into the effective profile is left
/// to EffectiveProfile::notify_base_profile_overrided(), which is still empty
/// -- so today this reorders the thread without actually changing its weight.
/// Deliberately not calling Profile::recompute_effective_profile() instead:
/// that one is declared but has no definition anywhere in the tree, so calling
/// it would trade a working build for a link error.
auto Fair::override_thread_base_profile(RunQueue &runtime, Thread *thread,
                                        BaseProfile const &profile) -> Status {
    if (profile.discipline != Discipline::Normal) {
        // A different discipline is a different schedule's thread; moving it
        // is the Scheduler's job, not ours.
        return Status::Unsupported;
    }

    auto &rq = runtime.queue<Discipline::Normal>();
    rq.verify();

    auto &ctx = thread->scheduling_entity();
    auto const queued = ctx.runqueue_state().is_active();
    if (queued) {
        update_thread(runtime, thread);
        update_vlag(runtime, ctx);
        rq.dequeue_object(ctx);
        rq.dec_queued();
    }

    ctx.m_profile.base = profile;
    ctx.m_profile.effective.notify_base_profile_overrided();

    if (queued) {
        // Re-place it: adjust_vlag() recomputes vruntime and vdeadline from the
        // weight it now has, which is the whole point of the round trip.
        adjust_vlag(runtime, ctx);
        rq.enqueue_object(ctx);
        rq.inc_queued();
    }

    return Status::Ok;
}

/// The one object of the discipline, and the reference the Scheduler is given to
/// it. Fair is defined in this file, so the reference is made here, where the
/// class is complete (Scheduler::schedule_of()).
namespace {
    constinit Fair FAIR_SCHEDULE;
} // namespace

auto fair_schedule() -> ustl::Ref<Schedule> {
    return ustl::Ref<Schedule>{FAIR_SCHEDULE};
}

} // namespace ours::task::sched