/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_RR_RUN_QUEUE_HPP
#define OURS_TASK_SCHED_RR_RUN_QUEUE_HPP 1

#include <ours/compiler.hpp>
#include <ours/task/sched/sched_entity.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::task::sched {
/// The round-robin discipline's per-CPU state: one FIFO of runnable contexts.
///
/// Held by RunQueue -- the per-CPU object a discipline is handed -- so that
/// RoundRobin reaches it straight off its own parameter instead of
/// recovering the whole Scheduler to find it. It is intrusive
/// through SchedEntity::m_rr_hook, a list hook distinct from the set hook Fair's
/// RunQueue uses: a SchedEntity is never on both queues, but the two containers
/// need different node shapes.
class RrRunQueue {
    using Self = RrRunQueue;

  public:
    FORCE_INLINE
    auto verify() const -> void {
        m_canary.verify<Self>();
    }

    /// Runnable contexts in arrival order; the head is the next to run.
    auto enqueue(SchedEntity &ctx) -> void;
    auto dequeue(SchedEntity &ctx) -> void;

    /// The context at the head of the FIFO, or null when it is empty.
    [[nodiscard]]
    auto peek() -> SchedEntity *;

    [[nodiscard]]
    FORCE_INLINE
    auto num_queued() const -> usize {
        return m_num_queued;
    }

    /// The first context, from the head, for which |pred| holds, or null when
    /// none does. For the generic scheduler, which picks threads out of the
    /// queue by something the FIFO does not order by -- whether they may run
    /// on another CPU, as this one goes offline -- and moves what it picks
    /// through the discipline's dequeue, which keeps the counts. Under the
    /// run queue lock; the result is good only while it is held.
    template <typename Pred>
    [[nodiscard]]
    auto find_first(Pred &&pred) -> SchedEntity * {
        for (auto &ctx: m_queue) {
            if (pred(ctx)) {
                return &ctx;
            }
        }
        return nullptr;
    }

  private:
    using Queue = ustl::collections::intrusive::List<SchedEntity, SchedEntity::RrQueueOption>;

    gktl::Canary m_canary{this};
    Queue m_queue;
    usize m_num_queued = 0;
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RR_RUN_QUEUE_HPP
