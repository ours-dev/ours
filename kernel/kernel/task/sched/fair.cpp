#include <ours/task/sched/config.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/sched/fair.hpp>
#include <ours/task/sched/features.hpp>
#include <ours/task/sched/runtime.hpp>
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
namespace {
    FORCE_INLINE
    auto calc_weighted_time(SchedDuration duration, Weight weight) -> SchedDuration {
        if (weight == MID_WEIGHT) {
            return duration;
        }

        auto const rate = weight / MID_WEIGHT;
        return duration * rate;
    }

} // namespace

class Observer {
    using Self = Runtime;

  public:
    static auto on_insert() -> void;

    static auto on_erase() -> void;

    static auto on_rotate(Context &before, Context &after) -> void;

    static auto propagate(Context &object, Context &terminal) -> void;

    static auto copy(Context &from, Context &to) -> void;
};

/// RunQueue itself -- all of Fair's per-CPU state -- is declared in
/// fair.hpp and held directly by Scheduler (Scheduler::fair_run_queue()).

auto RunQueue::calc_weighted_time(Context &object) const -> SchedDuration {
    return sched::calc_weighted_time(object.runtime() - min_vruntime_, object.weight());
}

auto RunQueue::dequeue_object(Context &object) -> void {
    (void)queue_.erase(queue_.iterator_to(object));

    weight_sum_ -= object.weight();
    total_vruntime_ -= calc_weighted_time(object);
}

auto RunQueue::enqueue_object(Context &object) -> void {
    (void)queue_.insert(object);

    weight_sum_ += object.weight();
    total_vruntime_ += calc_weighted_time(object);
}

/// RelativeWeightedOwnedVirtualTime(i) = WeightedOwnedVirtualTime(i) - MinimalOwnedVirtualTime;
/// RelativeWeightedOwnedVirtualTimeSum = \Sum RelativeWeightedOwnedVirtualTime(i)
/// WeightedOwnedVirtualTimeSum = RelativeWeightedOwnedVirtualTimeSum + MinimalOwnedVirtualTime * NumTasks
///
/// If MinimalOwnedVirtualTime will be plus Delta, then
///     RelativeWeightedOwnedVirtualTime(i) -= Delta
/// ctx that
///     RelativeWeightedOwnedVirtualTimeSum -= Delta * TotalWeight
auto RunQueue::update_min_vruntime(Context &current) -> void {
    SchedDuration min_vruntime;
    if (current.runtime() < min_vruntime_) {
        min_vruntime = current.runtime();
    } else {
        min_vruntime = min_vruntime_;
    }

    // TODO(SmallHuaZi) To figure out the problem that need to search in runqueue
    // for a potential minimal vruntime.

    if (auto delta = min_vruntime - min_vruntime_; delta > SchedDuration{0}) {
        min_vruntime_ = min_vruntime; // min_vruntime_ + delta
        total_vruntime_ -= delta * weight_sum_;
    }
}

/// Stateless: implements the EEVDF algorithm only. All per-CPU state lives
/// in RunQueue (fair.hpp), held by Scheduler -- one shared Fair instance
/// (see Schedule<Fair, ...>::REGISTRATION) serves every CPU, fetching the
/// calling CPU's RunQueue via Scheduler::Current::get().fair_run_queue().
class Fair final: public Schedule<Fair, Discipline::Normal> {
    using Self = Fair;

  public:
    auto enqueue_thread(Runtime &, Thread *, EnqueueControl) -> Status override;
    auto dequeue_thread(Runtime &, Thread *, DequeueControl) -> Status override;

    auto yield_thread(Runtime &, Thread *) -> void override;
    auto yield_to_thread(Runtime &, Thread *, Thread *) -> Status override;

    auto evaluate_next_thread(Runtime &, Thread *) -> Thread * override;
    auto put_prev_thread(Runtime &, Thread *) -> void override;
    auto set_next_thread(Runtime &, Thread *) -> void override;

    auto update_thread(Runtime &, Thread *curr) -> void override;

    auto on_thread_init(Runtime &, Thread *) -> void override;
    auto on_thread_dead(Runtime &, Thread *) -> void override;
    auto on_thread_unpark(Runtime &, Thread *) -> void override;
    auto on_thread_tick(Runtime &, Thread *, bool queued) -> void override;
    auto unpark_preempt(Runtime &runtime, Thread *thread, Thread *curr, UnparkControl)
        -> Status override;

    /// Not overrides of ISchedule::migrate_thread/select_target_cpu -- Fair's
    /// versions need the per-CPU Runtime that only this discipline's callers
    /// have on hand. Brought into Fair's overload set via the `using`
    /// declarations below so the differing signatures don't hide the base
    /// virtuals (-Woverloaded-virtual); the two APIs are deliberately left
    /// unreconciled for now.
    using ISchedule::migrate_thread;
    using ISchedule::select_target_cpu;
    auto migrate_thread(Runtime &, Thread *, CpuNum) -> Status;
    auto select_target_cpu(Runtime &, Thread *) -> CpuNum;

    auto override_thread_base_profile(Runtime &, Thread *, BaseProfile const &) -> Status override;

    [[nodiscard]]
    auto num_queued(Runtime &runtime) const -> usize override {
        return Scheduler::of(runtime).fair_run_queue().num_queued();
    }

  private:
    auto try_to_delay_dequeue(Runtime &, Context &, DequeueControl) -> bool;

    auto is_object_eligible(Context const &) const -> bool;

    auto adjust_vlag(Runtime &runtime, Context &object) -> void;

    auto update_vlag(Context &) -> void;

    auto set_next_object(Context &object) -> void;
};

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
FORCE_INLINE
auto Fair::is_object_eligible(Context const &object) const -> bool {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    auto const total_vruntime = rq.total_vruntime();
    auto const weight_sum = rq.total_weight();

    return total_vruntime >= ((object.runtime() - rq.min_vruntime()) * weight_sum);
}

auto Fair::adjust_vlag(Runtime &runtime, Context &ctx) -> void {
    auto ideal_vruntime = Scheduler::Current::get().fair_run_queue().avg_vruntime();
    auto vlag = ctx.m_params.vlag;

    auto &timeslice = ctx.m_timeslice;
    if (!timeslice.is_custom_timeslice()) {
        timeslice.m_base = static_cast<u32>(scheduling_period().raw());
    }

    auto &params = ctx.m_params;
    if (vlag != SchedDuration{0}) {
        // Adjust the vlag.
    }

    // Compensates the |object|
    params.vruntime = ideal_vruntime - vlag;
    params.vdeadline = params.vruntime + calc_weighted_time(timeslice.length(), ctx.weight());
}

FORCE_INLINE
auto Fair::update_vlag(Context &object) -> void {
    object.m_params.vlag = Scheduler::Current::get().fair_run_queue().lag_of(object);
}

auto Fair::update_thread(Runtime &runtime, Thread *curr) -> void {
    Scheduler::Current::get().fair_run_queue().verify();

    auto const now = runtime.clock();
    auto const elapsed = now - runtime.curr_started_time;

    if (elapsed < SchedDuration{0}) [[unlikely]] {
        return;
    }

    auto &ctx = curr->scheduling_context();

    runtime.curr_started_time = now;
    ctx.m_params.runtime += elapsed;
    ctx.m_params.vruntime += calc_weighted_time(elapsed, ctx.weight());

    if (ctx.deadline() <= ctx.runtime()) {
        // Note it has completely consumed it's request, ctx there is an occassion to preempt.
        runtime.mark_preempt_pending();

        ctx.m_params.vdeadline =
            ctx.m_params.vruntime + calc_weighted_time(ctx.m_timeslice.length(), ctx.weight());
    }
}

auto Fair::try_to_delay_dequeue(Runtime &runtime, Context &ctx, DequeueControl control) -> bool {
    if (!!(DequeueControl::Delayed & control)) {
        // Check the |ctx| is a delayed scheduling object.
        return false;
    }

    auto delayable = !!(DequeueControl::Sleep & control);
    if (sched::has_feature(Feature::DelayDequeue) && delayable && !is_object_eligible(ctx)) {
        return true;
    }

    return false;
}

auto Fair::dequeue_thread(Runtime &runtime, Thread *thread, DequeueControl control) -> Status {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    auto const current_thread = runtime.curr_thread;
    auto const is_current_thread = thread == current_thread;
    update_thread(runtime, current_thread);

    auto &ctx = thread->scheduling_context();
    if constexpr (SCHED_DELAY_DEQUEUE) {
        if (try_to_delay_dequeue(runtime, ctx, control)) {
            return Status::Delayed;
        }
    }

    update_vlag(ctx);
    if (!is_current_thread) {
        rq.dequeue_object(ctx);
        rq.dec_queued();
    }
    ctx.runqueue_state().on_dequeue();

    return Status::Ok;
}

auto Fair::enqueue_thread(Runtime &runtime, Thread *thread, EnqueueControl control) -> Status {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    auto const current_thread = runtime.curr_thread;
    auto const is_current_thread = thread == current_thread;

    auto &ctx = thread->scheduling_context();

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

auto Fair::on_thread_tick(Runtime &runtime, Thread *curr, bool queued) -> void {
    Scheduler::Current::get().fair_run_queue().verify();
    update_thread(runtime, curr);

    if (queued) {
        curr->preemption_state().add_pending();
    }
}

auto Fair::on_thread_init(Runtime &, Thread *thread) -> void {
    Scheduler::Current::get().fair_run_queue().verify();
    [[maybe_unused]]
    auto &ctx = thread->scheduling_context();
}

auto Fair::on_thread_dead(Runtime &, Thread *thread) -> void {
    Scheduler::Current::get().fair_run_queue().verify();
    [[maybe_unused]]
    auto &ctx = thread->scheduling_context();
}

/// The thread has just been made runnable again. By the time this runs
/// Scheduler::unpark() has already enqueued it, so placement -- the vlag
/// compensation in adjust_vlag() -- is done; what is left is the bookkeeping
/// that only the wakeup path knows.
auto Fair::on_thread_unpark(Runtime &runtime, Thread *thread) -> void {
    Scheduler::Current::get().fair_run_queue().verify();

    // Where the wakeup landed. Nothing else records this, and it is what a
    // future select_target_cpu() needs to keep a woken thread near the cpu
    // that woke it rather than scattering it across the machine.
    thread->scheduling_context().m_wake_cpu = runtime.which_cpu;
}

/// In order to provide latency guarantees for different request sizes
/// EEVDF selects the best runnable task from two criteria:
///  (1) the task must be eligible (must be owed service)
///
///  (2) from those tasks that meet (1), we select the one
///      with the earliest virtual deadline.
auto Fair::evaluate_next_thread(Runtime &runtime, Thread *curr) -> Thread * {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();
    auto iter = rq.begin();
    for (auto const last = rq.end(); iter != last; ++iter) {
        if (is_object_eligible(*iter)) {
            break;
        }
    }

    if (iter == rq.end()) {
        return 0;
    }

    return Thread::of(&*iter);
}

auto Fair::put_prev_thread(Runtime &runtime, Thread *prev) -> void {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    auto &ctx = prev->scheduling_context();

    // The thread |prev| was not been deactivated via Scheduler::deactivate_thread().
    // Therefore now it is still on our runqueue.
    if (ctx.runqueue_state().is_active()) {
        update_thread(runtime, prev);
        rq.enqueue_object(ctx);
    }
}

auto Fair::set_next_object(Context &ctx) -> void {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    // The next object may not be on queue, ctx it demands us to check as the following to
    // prevent double dequeue operations.
    if (ctx.runqueue_state().is_active()) {
        rq.dequeue_object(ctx);
    }

    ctx.m_params.prev_runtime = ctx.m_params.runtime;
    ctx.m_params.vdeadline =
        ctx.m_params.vruntime + calc_weighted_time(ctx.m_timeslice.length(), ctx.weight());
}

auto Fair::set_next_thread(Runtime &runtime, Thread *next) -> void {
    Scheduler::Current::get().fair_run_queue().verify();

    auto &ctx = next->scheduling_context();
    set_next_object(ctx);

    if constexpr (SCHED_NOHZ) {
        // When configured NOHZ, need we to manuaully reset the preemption time point.
        // It is possiablely that the next thread has been woken just now, ctx recalculate
        //
        auto const consumed_timeslice = ctx.m_params.runtime - ctx.m_params.prev_runtime;
        auto const remaining_timeslice = ctx.m_timeslice.length() - consumed_timeslice;
        if (remaining_timeslice < SchedDuration{0}) {
            return;
        }

        runtime.driver.reset_preempt(remaining_timeslice);
    }
}

/// |curr| voluntarily gives up the cpu.
///
/// Under EEVDF the pick is "the eligible object with the earliest virtual
/// deadline", so yielding is not a requeue -- it is giving up that earliest
/// deadline. Pushing the deadline out by one whole request is what lets some
/// other runnable thread win the next pick, while leaving eligibility (and
/// therefore long-run fairness) untouched.
auto Fair::yield_thread(Runtime &runtime, Thread *curr) -> void {
    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    // Nothing else to yield to; charging time and moving the deadline would
    // only penalise |curr| for no one's benefit.
    if (!rq.num_queued()) {
        return;
    }

    // Charge what it has used so far, so the deadline is pushed out from an
    // up-to-date vruntime rather than a stale one.
    update_thread(runtime, curr);

    auto &ctx = curr->scheduling_context();
    ctx.m_params.vdeadline += calc_weighted_time(ctx.m_timeslice.length(), ctx.weight());

    runtime.mark_preempt_pending();
}

auto Fair::yield_to_thread(Runtime &runtime, Thread *curr, Thread *next) -> Status {
    Scheduler::Current::get().fair_run_queue().verify();

    if (!next->scheduling_context().runqueue_state().is_active()) {
        // Received a inactive thread.
        return Status::BadState;
    }

    update_thread(runtime, curr);
    runtime.next_thread = next;
    return Status::Ok;
}

auto Fair::unpark_preempt(Runtime &runtime, Thread *thread, Thread *curr, UnparkControl) -> Status {
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
auto Fair::override_thread_base_profile(Runtime &runtime, Thread *thread,
                                        BaseProfile const &profile) -> Status {
    if (profile.discipline != Discipline::Normal) {
        // A different discipline is a different schedule's thread; moving it
        // is the Scheduler's job, not ours.
        return Status::Unsupported;
    }

    auto &rq = Scheduler::Current::get().fair_run_queue();
    rq.verify();

    auto &ctx = thread->scheduling_context();
    auto const queued = ctx.runqueue_state().is_active();
    if (queued) {
        update_thread(runtime, thread);
        update_vlag(ctx);
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

/// Required, not decorative: deriving Fair from Schedule<Fair, ...> only
/// instantiates the base's member *declarations*. A static data member of a
/// class template is instantiated only when odr-used or explicitly
/// instantiated -- so without this, `REGISTRATION` is never emitted, the
/// linker collects nothing, and Scheduler::init() finds an empty section.
/// `[[gnu::used]]` cannot substitute: it keeps an emitted symbol alive, but
/// cannot cause one to be instantiated.
template class Schedule<Fair, Discipline::Normal>;

} // namespace ours::task::sched