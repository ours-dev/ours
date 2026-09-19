/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/task/sched/rr.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

namespace ours::task::sched {
namespace {
    /// The one place the queue is looked up, so every override agrees on which
    /// CPU's it is: the one that owns the RunQueue the entry point was handed.
    ///
    /// It used to be `Scheduler::Current::get()`, the *running* CPU's, on the
    /// stated assumption that "every ISchedule entry point runs on the CPU
    /// whose RunQueue it was handed". That is not true and cannot be made true:
    /// Scheduler::do_unpark_thread() deliberately runs on the waking CPU and
    /// inserts into the target CPU's run queue under the target's lock, which
    /// is how a thread reaches a CPU that is halted in its idle loop. With the
    /// running CPU's queue, a cross-CPU wakeup incremented the target's
    /// num_queued_threads while putting the thread on the waker's own FIFO --
} // namespace

/// Charge |curr| for the time since it was last charged. This is the whole
/// accounting model: elapsed time goes into m_exec_runtime, and the slice is
/// spent when that passes m_timeslice.length().
///
/// `curr_started_time` is the discipline's to maintain -- nothing in the
/// generic Scheduler writes it -- and it is reset here rather than only in
/// set_next_thread() so that a thread charged mid-slice is not charged for the
/// same interval twice.
///
/// A member rather than a file-local helper because it reads SchedEntity's private
/// accounting fields and Thread's private scheduling_entity(); the friendship
/// both classes extend to RoundRobin does not reach a free function.
auto RoundRobin::charge(RunQueue &rq, Thread *curr) -> void {
    auto const elapsed = rq.charge_current();
    if (elapsed == SchedDuration{0}) [[unlikely]] {
        return;
    }

    curr->scheduling_entity().m_exec_runtime += elapsed;
}

auto RoundRobin::slice_spent(SchedEntity &ctx) -> bool {
    return ctx.m_exec_runtime - ctx.m_prev_exec_runtime >= ctx.m_timeslice.length();
}

auto RrRunQueue::enqueue(SchedEntity &ctx) -> void {
    m_queue.push_back(ctx);
    m_num_queued += 1;
}

auto RrRunQueue::dequeue(SchedEntity &ctx) -> void {
    m_queue.erase(ctx);
    m_num_queued -= 1;
}

auto RrRunQueue::peek() -> SchedEntity * {
    auto head = m_queue.front();
    return head ? &head.value() : nullptr;
}

auto RoundRobin::enqueue_thread(RunQueue &rq, Thread *thread, EnqueueOptions) -> Status {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    auto &ctx = thread->scheduling_entity();

    // The running thread is not on the FIFO -- set_next_thread() took it off
    // -- so an enqueue of it is a request to re-queue, which put_prev_thread()
    // is the one to honour. Being asked here is a no-op, not an error.
    if (thread == rq.current_thread()) {
        return Status::Ok;
    }

    queue.enqueue(ctx);
    ctx.runqueue_state().on_enqueue();
    return Status::Ok;
}

auto RoundRobin::dequeue_thread(RunQueue &rq, Thread *thread, DequeueOptions) -> Status {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    auto &ctx = thread->scheduling_entity();

    // Same asymmetry as enqueue: the running thread already left the FIFO at
    // set_next_thread(), so the only thing to record is that it is no longer
    // queueable. Charging it first keeps its rq honest for whoever reads
    // it later.
    if (thread == rq.current_thread()) {
        charge(rq, thread);
    } else if (ctx.runqueue_state().is_active()) {
        queue.dequeue(ctx);
    }
    ctx.runqueue_state().on_dequeue();
    return Status::Ok;
}

auto RoundRobin::evaluate_next_thread(RunQueue &rq, Thread *) -> Thread * {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    auto *const head = queue.peek();
    return head ? Thread::of(head) : nullptr;
}

/// |prev| is leaving the CPU. If it is still runnable it goes to the back of
/// the FIFO -- that is the rotation that makes this round-robin -- and its
/// slice is charged up to now first so the next set_next_thread() starts it
/// from a clean baseline.
auto RoundRobin::put_prev_thread(RunQueue &rq, Thread *prev) -> void {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    auto &ctx = prev->scheduling_entity();
    if (!ctx.runqueue_state().is_active()) {
        // Parked, sleeping, dead: dequeue_thread() already recorded it.
        return;
    }

    charge(rq, prev);
    queue.enqueue(ctx);
}

/// |next| is about to run. It leaves the FIFO -- the running thread is not a
/// member, see enqueue_thread() -- and its slice starts now: prev_runtime is
/// the baseline slice_spent() measures from, and curr_started_time is where
/// charge() measures elapsed time from.
auto RoundRobin::set_next_thread(RunQueue &rq, Thread *next) -> void {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    auto &ctx = next->scheduling_entity();
    if (ctx.runqueue_state().is_active()) {
        queue.dequeue(ctx);
    }

    ctx.m_prev_exec_runtime = ctx.m_exec_runtime;
    rq.restart_current_slice();
}

/// Called on every clock update for the running thread. Once its slice is
/// spent, ask for a reschedule; put_prev_thread() then rotates it to the back.
/// A lone runnable thread keeps running regardless -- there is nobody to
/// rotate to, and asking for a reschedule would only pay for a switch back to
/// itself.
auto RoundRobin::update_thread(RunQueue &rq, Thread *curr) -> void {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    charge(rq, curr);

    if (queue.num_queued() && slice_spent(curr->scheduling_entity())) {
        rq.mark_preempt_pending();
    }
}

/// Yielding under round-robin is giving up the rest of the slice: mark it
/// spent and let put_prev_thread() rotate. Pointless with nobody else queued.
auto RoundRobin::yield_thread(RunQueue &rq, Thread *curr) -> void {
    auto &queue = rq.queue<Discipline::Normal>();
    queue.verify();

    if (!queue.num_queued()) {
        return;
    }

    charge(rq, curr);
    rq.mark_preempt_pending();
}

auto RoundRobin::yield_to_thread(RunQueue &rq, Thread *curr, Thread *next) -> Status {
    rq.queue<Discipline::Normal>().verify();

    if (!next->scheduling_entity().runqueue_state().is_active()) {
        return Status::BadState;
    }

    charge(rq, curr);
    rq.nominate_next_thread(next);
    return Status::Ok;
}

/// The slice is the RunQueue's scheduling period, fixed at init because nothing
/// in this discipline ever scales it. Timeslice::length() would otherwise read
/// a zero base and slice_spent() would be true from the first tick.
auto RoundRobin::on_thread_init(RunQueue &rq, Thread *thread) -> void {
    auto &timeslice = thread->scheduling_entity().m_timeslice;
    if (!timeslice.is_custom_timeslice()) {
        timeslice.m_base = static_cast<u32>(scheduling_period().raw());
    }
}

/// Pure virtual in ISchedule, so it has to exist; the generic Scheduler's tick
/// path goes through update_thread() instead and never calls this.
auto RoundRobin::on_thread_tick(RunQueue &, Thread *, bool) -> void {}

auto RoundRobin::num_queued(RunQueue &rq) const -> usize {
    return rq.queue<Discipline::Normal>().num_queued();
}

/// Required, not decorative -- see the identical block at the end of fair.cpp:
/// without an explicit instantiation the Schedule<> base's static
/// REGISTRATION member is never emitted, the linker collects nothing into
/// .task.schedules, and Scheduler::init() finds no discipline at all.
template class Schedule<RoundRobin, Discipline::Normal>;

} // namespace ours::task::sched
