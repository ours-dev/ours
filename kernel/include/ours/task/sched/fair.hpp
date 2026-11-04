/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_FAIR_HPP
#define OURS_TASK_SCHED_FAIR_HPP 1

#include <ours/compiler.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/sched/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/set.hpp>

namespace ours::task::sched {
/// Fair (EEVDF) discipline's per-CPU run queue -- all of the Fair
/// discipline's per-CPU state. Held directly by the per-CPU RunQueue (see
/// RunQueue::queue<Discipline::Normal>()) rather than through its own CpuLocal
/// instance: the RunQueue is already per CPU, so a second, independent
/// per-CPU mechanism just for this queue would be redundant.
///
/// This mirrors Linux's `struct rq` embedding `struct cfs_rq cfs`
/// directly rather than routing through a generic/type-erased slot: the
/// set of disciplines is small and fixed at compile time (see
/// `Discipline`), so the per-CPU container naming its disciplines' queues
/// directly is simpler than full type erasure. The `Schedule` interface
/// already decouples the *behavior* (Fair's algorithm) from Scheduler;
/// this only concerns where the *data* lives.
class FairRunQueue {
    using Self = FairRunQueue;

  public:
    FORCE_INLINE
    auto verify() const -> void {
        m_canary.verify<Self>();
    }

    auto enqueue_object(SchedEntity &object) -> void;

    auto dequeue_object(SchedEntity &object) -> void;

    /// |object| is the thread now running on this CPU. It is off the tree, and
    /// part of the average for as long as it is active (`sums()`).
    FORCE_INLINE
    auto set_current(SchedEntity &object) -> void {
        current_ = &object;
    }

    /// The running thread is going back to the tree, or has stopped wanting
    /// to run.
    FORCE_INLINE
    auto clear_current() -> void {
        current_ = nullptr;
    }

    /// The weighted mean of the virtual times of everything that wants to run
    /// here, the running thread included. It is the point of the queue at which
    /// a thread owes nothing and is owed nothing, and a thread is placed on it
    /// less the lag it carried.
    ///
    /// Moves the queue's zero to the mean on the way (`recenter()`), which is
    /// why it is not const. An empty queue has no mean and answers its zero.
    auto avg_vruntime() -> SchedDuration;

    /// Moves the zero to the weighted mean. The sums are valid for any zero, but
    /// an offset converts into `WeightedTime` only up to about 39 hours, so the
    /// zero has to follow the threads: it is moved at every placement and lag
    /// (`avg_vruntime()`) and whenever a request is replenished
    /// (`Fair::update_thread()`), which is what keeps a thread that is only ever
    /// re-picked from running away from it.
    auto recenter() -> void;

    /// Returns the lag of |object| relative to this runqueue.
    FORCE_INLINE
    auto lag_of(SchedEntity &object) -> SchedDuration {
        return avg_vruntime() - object.vruntime();
    }

    /// Whether |object| is owed service: its lag is not negative.
    ///
    /// The sum and the weights it is tested against are `sums()`, the same pair
    /// `avg_vruntime()` uses, so the running thread is in both.
    [[nodiscard]]
    auto is_eligible(SchedEntity const &object) const -> bool;

    /// The weights of what is in the mean: the tree and the running thread.
    FORCE_INLINE
    auto total_weight() const -> Weight {
        return sums().weight;
    }

    /// The weighted sum of what is in the mean: the tree and the running thread.
    FORCE_INLINE
    auto total_vruntime() const -> WeightedTime {
        return sums().total;
    }

    FORCE_INLINE
    auto zero_vruntime() const -> SchedDuration {
        return zero_vruntime_;
    }

    FORCE_INLINE
    auto begin() {
        return queue_.begin();
    }

    FORCE_INLINE
    auto end() {
        return queue_.end();
    }

    FORCE_INLINE
    auto num_queued() const -> usize {
        return m_num_queued;
    }

    /// Tracks how many threads the scheduler considers logically queued.
    /// Deliberately separate from enqueue_object/dequeue_object -- those
    /// also run for a thread that's merely being repositioned around a
    /// run (see Fair::put_prev_thread/set_next_object), which must not
    /// move this count.
    FORCE_INLINE
    auto inc_queued() -> void {
        m_num_queued += 1;
    }

    FORCE_INLINE
    auto dec_queued() -> void {
        m_num_queued -= 1;
    }

  private:
    /// The weighted sum and the weights of what is in the mean: the tree, and
    /// the running thread while it is active.
    struct Sums {
        WeightedTime total;
        Weight weight;
    };
    [[nodiscard]]
    auto sums() const -> Sums;

    /// |object|'s term in the weighted sum, see sched::weighted_offset().
    auto term_of(SchedEntity &object) const -> WeightedTime;

    /// Moves the zero by |delta|, and the sum of the tree with it.
    auto update_zero_vruntime(SchedDuration const delta) -> void;

    /// Earliest virtual deadline first; the pick takes the first eligible one
    /// (Fair::evaluate_next_thread()).
    using Lesser = decltype([](SchedEntity const &x, SchedEntity const &y) noexcept {
        return x.virtual_deadline() < y.virtual_deadline();
    });
    using Queue = ustl::collections::intrusive::MultiSet<
        SchedEntity, ustl::collections::intrusive::ValueTraits<SchedEntity::RunQueueTraits<>>,
        ustl::collections::intrusive::Compare<Lesser>>;

    gktl::Canary m_canary{this};
    Queue queue_;
    /// The thread running on this CPU if it is one of Fair's. Not in `queue_`.
    SchedEntity *current_ = nullptr;
    /// The weights of what is in `queue_`.
    Weight weight_sum_;
    /// `weight * (vruntime - zero_vruntime_)` summed over what is in `queue_`.
    WeightedTime total_vruntime_;
    /// The point the offsets are taken from. A thread's term is its distance
    /// from it, so any value would do for the sums; keeping it at the mean
    /// keeps them small.
    SchedDuration zero_vruntime_;
    usize m_num_queued = 0;
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_FAIR_HPP
