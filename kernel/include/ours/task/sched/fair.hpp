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

    auto update_min_vruntime(SchedEntity &object) -> void;

    /// Returns the lag of |object| relative to this runqueue.
    FORCE_INLINE
    auto lag_of(SchedEntity &object) -> SchedDuration {
        return avg_vruntime() - object.exec_runtime();
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
    auto calc_weighted_time(SchedEntity &object) const -> SchedDuration;

    using Lesser = decltype([](SchedEntity const &x, SchedEntity const &y) noexcept {
        return x.deadline() < y.deadline();
    });
    using Queue = ustl::collections::intrusive::MultiSet<
        SchedEntity, ustl::collections::intrusive::ValueTraits<SchedEntity::RunQueueTraits<>>,
        ustl::collections::intrusive::Compare<Lesser>>;

    gktl::Canary m_canary{this};
    Queue queue_;
    SchedEntity *current_;
    Weight weight_sum_;
    SchedDuration total_vruntime_;
    SchedDuration min_vruntime_;
    usize m_num_queued = 0;
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_FAIR_HPP
