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
/// no arithmetic to get wrong: run the head of the FIFO for a fixed slice,
/// which ends at the CPU's preemption deadline, then move it to the back. There is no weighting, no lag, no deadline. The Fair (EEVDF)
/// discipline is the intended one; it is out of the build while the boot is
/// being walked, not deleted, and registering it again under
/// Discipline::Normal is how it comes back.
///
/// Only the parts of Schedule the generic Scheduler actually drives are
/// implemented; the optional methods it leaves out are answered by their
/// defaults. Its RunQueue bookkeeping -- curr_started_time in particular --
/// is the discipline's to maintain (nothing outside Fair touched it before),
/// so this one keeps it too.
class RoundRobin final {
    using Self = RoundRobin;

  public:
    TA_REQUIRES(rq.lock())
    auto enqueue_thread(RunQueue &rq, Thread *, EnqueueOptions) -> Status;
    TA_REQUIRES(rq.lock())
    auto dequeue_thread(RunQueue &rq, Thread *, DequeueOptions) -> Status;

    TA_REQUIRES(rq.lock())
    auto yield_thread(RunQueue &rq, Thread *) -> void;
    TA_REQUIRES(rq.lock())
    auto yield_to_thread(RunQueue &rq, Thread *, Thread *) -> Status;

    TA_REQUIRES(rq.lock())
    auto evaluate_next_thread(RunQueue &rq, Thread *) -> Thread *;
    TA_REQUIRES(rq.lock())
    auto put_prev_thread(RunQueue &rq, Thread *) -> void;
    TA_REQUIRES(rq.lock())
    auto set_next_thread(RunQueue &rq, Thread *) -> void;

    auto on_thread_init(RunQueue &, Thread *) -> void;
    auto on_thread_tick(RunQueue &, Thread *, bool queued) -> void;

    [[nodiscard]]
    TA_REQUIRES(rq.lock()) auto num_queued(RunQueue &rq) const -> usize;

  private:
    /// Charge |curr| for the time since it was last charged.
    static auto charge(RunQueue &, Thread *curr) -> void;
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_RR_HPP
