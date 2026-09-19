/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_RR_HPP
#define OURS_TASK_SCHED_RR_HPP 1

#include <ours/task/sched/rr_run_queue.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/sched/schedule.hpp>

#include <gktl/canary.hpp>

namespace ours::task::sched {
/// A deliberately plain round-robin discipline for bring-up.
///
/// It exists so the rest of the kernel can be brought up on a scheduler with
/// no arithmetic to get wrong: run the head of the FIFO, charge it against a
/// fixed slice on every clock update, and when the slice is spent move it to
/// the back. There is no weighting, no lag, no deadline. The Fair (EEVDF)
/// discipline is the intended one; it is out of the build while the boot is
/// being walked, not deleted, and registering it again under
/// Discipline::Normal is how it comes back.
///
/// Only the parts of ISchedule the generic Scheduler actually drives are
/// implemented. Its RunQueue bookkeeping -- curr_started_time in particular --
/// is the discipline's to maintain (nothing outside Fair touched it before),
/// so this one keeps it too.
class RoundRobin final: public Schedule<RoundRobin, Discipline::Normal> {
    using Self = RoundRobin;

  public:
    auto enqueue_thread(RunQueue &, Thread *, EnqueueOptions) -> Status override;
    auto dequeue_thread(RunQueue &, Thread *, DequeueOptions) -> Status override;

    TA_REQUIRES(run_queue.lock())
    auto yield_thread(RunQueue &run_queue, Thread *) -> void override;
    TA_REQUIRES(run_queue.lock())
    auto yield_to_thread(RunQueue &run_queue, Thread *, Thread *) -> Status override;

    TA_REQUIRES(run_queue.lock())
    auto evaluate_next_thread(RunQueue &run_queue, Thread *) -> Thread * override;
    TA_REQUIRES(run_queue.lock())
    auto put_prev_thread(RunQueue &run_queue, Thread *) -> void override;
    TA_REQUIRES(run_queue.lock())
    auto set_next_thread(RunQueue &run_queue, Thread *) -> void override;

    TA_REQUIRES(run_queue.lock())
    auto update_thread(RunQueue &run_queue, Thread *curr) -> void override;

    auto on_thread_init(RunQueue &, Thread *) -> void override;
    auto on_thread_tick(RunQueue &, Thread *, bool queued) -> void override;

    [[nodiscard]]
    TA_REQUIRES(run_queue.lock()) auto num_queued(RunQueue &run_queue) const -> usize override;

  private:
    /// Charge |curr| for the time since it was last charged.
    static auto charge(RunQueue &, Thread *curr) -> void;

    /// Whether |ctx| has used up the slice it was given at set_next_thread().
    [[nodiscard]]
    static auto slice_spent(SchedEntity &ctx) -> bool;
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RR_HPP
