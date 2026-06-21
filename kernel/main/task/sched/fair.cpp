#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/runtime.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/sched/features.hpp>
#include <ours/task/sched/config.hpp>

#include <ours/task/thread.hpp>

#include <ours/time/timer-queue.hpp>
#include <ours/time/tick-device.hpp>

#include <ustl/ratio.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/collections/intrusive/set.hpp>

#include <logz4/log.hpp>

using namespace ustl::collections::intrusive;

namespace ours::task::sched {
namespace {
    FORCE_INLINE
    auto calc_weighted_time(SchedDuration duration, Weight weight) -> SchedDuration {
        if (weight == kMidWeight) {
            return duration;
        }

        auto const rate = weight / kMidWeight;
        return duration * rate;
    }

} // namespace anonymous

    class Observer {
        using Self = Runtime;
      public:
        static auto on_insert() -> void;

        static auto on_erase() -> void;

        static auto on_rotate(Context &before, Context &after) -> void;

        static auto propagate(Context &object, Context &terminal) -> void;

        static auto copy(Context &from, Context &to) -> void;
    };

    class RunQueue {
        using Self = RunQueue;
      public:
        auto enqueue_object(Context &object) -> void;

        auto dequeue_object(Context &object) -> void;

        auto update_min_vruntime(Context &object) -> void;

        /// Returns the lag of |object| relative to this runqueue.
        FORCE_INLINE
        auto lag_of(Context &object) -> SchedDuration {
            return avg_vruntime() - object.runtime();
        }

        FORCE_INLINE
        auto total_weight() const -> Weight {
            if (current_ && current_->runqueue_state().is_active()) {
                return weight_sum_ + current_->weight();
            }
            return weight_sum_;
        }

        FORCE_INLINE
        auto total_vruntime() const -> SchedDuration {
            return total_vruntime_;
        }

        FORCE_INLINE
        auto min_vruntime() const -> SchedDuration {
            return min_vruntime_;
        }

        FORCE_INLINE
        auto avg_vruntime() const -> SchedDuration {
            auto total_vruntime = total_vruntime_;
            auto weight_sum = weight_sum_;

            if (current_ && current_->runqueue_state().is_active()) {
                weight_sum += current_->weight();
                total_vruntime += calc_weighted_time(*current_);
            }

            return min_vruntime_ + total_vruntime / weight_sum;
        }

        FORCE_INLINE
        auto begin() {
            return queue_.begin();
        }

        FORCE_INLINE
        auto end() {
            return queue_.end();
        }
      private:
        FORCE_INLINE
        auto calc_weighted_time(Context &object) const -> SchedDuration {
            return sched::calc_weighted_time(object.runtime() - min_vruntime_, object.weight());
        }

        using Lesser = decltype([] (Context const &x, Context const &y) {
            return x.deadline() < y.deadline(); 
        });
        using Queue = MultiSet<Context, ValueTraits<Context::RunQueueTraits<>>, Compare<Lesser>>;
        Queue queue_;
        Context *current_;
        Weight weight_sum_;
        SchedDuration total_vruntime_;
        SchedDuration min_vruntime_;
    };

    auto RunQueue::dequeue_object(Context &object) -> void {
        queue_.erase(queue_.iterator_to(object));

        weight_sum_ -= object.weight();
        total_vruntime_ -= calc_weighted_time(object);
    }

    auto RunQueue::enqueue_object(Context &object) -> void {
        queue_.insert(object);

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

        if (auto delta = min_vruntime - min_vruntime_;  delta > SchedDuration::zero()) {
            min_vruntime_ = min_vruntime; // min_vruntime_ + delta
            total_vruntime_ -= delta * weight_sum_;
        }
    }

    [[=sched::ScheduleOp]]
    class Fair final: public Schedule {
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
        auto unpark_preempt(Runtime &runtime, Thread *thread, Thread *curr, UnparkControl) -> Status override;

        auto migrate_thread(Runtime &, Thread *, CpuNum) -> Status;
        auto select_target_cpu(Runtime &, Thread *) -> CpuNum;

        auto override_thread_base_profile(Runtime &, Thread *, BaseProfile const &) -> Status override;
      private:
        auto try_to_delay_dequeue(Runtime &, Context &, DequeueControl) -> bool;

        auto is_object_eligible(Context const &) const -> bool;

        auto adjust_vlag(Runtime &runtime, Context &object) -> void;

        auto update_vlag(Context &) -> void;

        auto set_next_object(Context &object) -> void;

        RunQueue m_runqueue;
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
        auto const total_vruntime = m_runqueue.total_vruntime();
        auto const weight_sum = m_runqueue.total_weight();

        return total_vruntime >= ((object.runtime() - m_runqueue.min_vruntime()) * weight_sum);
    }

    auto Fair::adjust_vlag(Runtime &runtime, Context &ctx) -> void {
        auto ideal_vruntime = m_runqueue.avg_vruntime();
        auto vlag = ctx.m_params.vlag;

        auto &timeslice = ctx.m_timeslice;
        if (!timeslice.is_custom_timeslice()) {
            timeslice.m_base = runtime.scheduling_period().count();
        }

        auto &params = ctx.m_params;
        if (vlag != SchedDuration::zero()) {
            // Adjust the vlag.
        }

        // Compensates the |object|
        params.vruntime = ideal_vruntime - vlag;
        params.vdeadline = params.vruntime + calc_weighted_time(timeslice.length(), ctx.weight());
    }

    FORCE_INLINE
    auto Fair::update_vlag(Context &object) -> void {
        object.m_params.vlag = m_runqueue.lag_of(object);
    }

    auto Fair::update_thread(Runtime &runtime, Thread *curr) -> void {
        m_canary.verify<Self>();

        auto const now = runtime.clock();
        auto const elapsed = now - runtime.curr_started_time;

        if (elapsed < SchedDuration::zero()) [[unlikely]] {
            return;
        }

        auto &ctx = curr->scheduling_context();

        runtime.curr_started_time = now;
        ctx.m_params.runtime += elapsed;
        ctx.m_params.vruntime += calc_weighted_time(elapsed, ctx.weight());

        if (ctx.deadline().time_since_epoch() <= ctx.runtime()) {
            // Note it has completely consumed it's request, ctx there is an occassion to preempt.
            runtime.mark_preempt_pending();

            ctx.m_params.vdeadline = ctx.m_params.vruntime + calc_weighted_time(ctx.m_timeslice.length(), ctx.weight());
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
        m_canary.verify<Self>();

        auto const current_thread = runtime.curr_thread;
        auto const is_current_thread = thread == current_thread;
        update_thread(runtime, current_thread);

        auto &ctx = thread->scheduling_context();
        if CXX20_CONSTEXPR (SCHED_DELAY_DEQUEUE) {
            if (try_to_delay_dequeue(runtime, ctx, control)) {
                return Status::Delayed;
            }
        }

        update_vlag(ctx);
        if (!is_current_thread) {
            m_runqueue.dequeue_object(ctx);
            m_num_queued -= 1;
        }
        ctx.runqueue_state().on_dequeue();

        return Status::Ok;
    }

    auto Fair::enqueue_thread(Runtime &runtime, Thread *thread, EnqueueControl control) -> Status {
        m_canary.verify<Self>();

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
            m_runqueue.enqueue_object(ctx);
            m_num_queued += 1;
        }
        ctx.runqueue_state().on_enqueue();

        return Status::Ok;
    }

    auto Fair::on_thread_tick(Runtime &runtime, Thread *curr, bool queued) -> void {
        m_canary.verify<Self>();
        update_thread(runtime, curr);

        if (queued) {
            curr->preemption_state().add_pending();
        }
    }

    auto Fair::on_thread_init(Runtime &, Thread *thread) -> void {
        m_canary.verify<Self>();
        auto &ctx = thread->scheduling_context();
    }

    auto Fair::on_thread_dead(Runtime &, Thread *thread) -> void {
        m_canary.verify<Self>();
        auto &ctx = thread->scheduling_context();
    }

    /// In order to provide latency guarantees for different request sizes
    /// EEVDF selects the best runnable task from two criteria:
    ///  (1) the task must be eligible (must be owed service)
    ///
    ///  (2) from those tasks that meet (1), we select the one
    ///      with the earliest virtual deadline.
    auto Fair::evaluate_next_thread(Runtime &runtime, Thread *curr) -> Thread * {
        m_canary.verify<Self>();
        auto iter = m_runqueue.begin();
        for (auto const last = m_runqueue.end(); iter != last; ++iter) {
            if (is_object_eligible(*iter)) {
                break;
            }
        }

        if (iter == m_runqueue.end()) {
            return 0;
        }

        return Thread::of(&*iter);
    }

    auto Fair::put_prev_thread(Runtime &runtime, Thread *prev) -> void {
        m_canary.verify<Self>();

        auto &ctx = prev->scheduling_context();

        // The thread |prev| was not been deactivated via Scheduler::deactivate_thread().
        // Therefore now it is still on our runqueue.
        if (ctx.runqueue_state().is_active()) {
            update_thread(runtime, prev);
            m_runqueue.enqueue_object(ctx);
        }
    }

    auto Fair::set_next_object(Context &ctx) -> void {
        m_canary.verify<Self>();

        // The next object may not be on queue, ctx it demands us to check as the following to
        // prevent double dequeue operations.
        if (ctx.runqueue_state().is_active()) {
            m_runqueue.dequeue_object(ctx);
        }

        ctx.m_params.prev_runtime = ctx.m_params.runtime;
        ctx.m_params.vdeadline = ctx.m_params.vruntime + calc_weighted_time(ctx.m_timeslice.length(), ctx.weight());
    }

    auto Fair::set_next_thread(Runtime &runtime, Thread *next) -> void {
        m_canary.verify<Self>();

        auto &ctx = next->scheduling_context();
        set_next_object(ctx);

        if CXX17_CONSTEXPR(OURS_CONFIG_NOHZ) {
            // When configured NOHZ, need we to manuaully reset the preemption time point.
            // It is possiablely that the next thread has been woken just now, ctx recalculate
            //  
            auto const consumed_timeslice = ctx.m_params.runtime - ctx.m_params.prev_runtime;
            auto const remaining_timeslice = ctx.m_timeslice.length() - consumed_timeslice;
            if (remaining_timeslice < decltype(remaining_timeslice)::zero()) {
                return;
            }

            runtime.driver.reset_preempt(remaining_timeslice);
        }
    }

    auto Fair::yield_to_thread(Runtime &runtime, Thread *curr, Thread *next) -> Status {
        m_canary.verify<Self>();

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
} // namespace ours::task::sched