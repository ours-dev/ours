/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

#include <arch/auto_interrupt_disabler.hpp>

namespace ours::sync {
using task::Scheduler;
using task::Thread;

/// Every post either wakes one waiter or, if none is queued, adds one to the
/// count -- never both, and never neither. That is what keeps "a thread waits
/// only while the count is zero": a unit handed to a waiter never touches
/// the count, so the count cannot rise while a thread is queued.
///
/// A waiter that times out or is interrupted takes itself off the queue
/// (WaitQueue::handle_timeout(), Thread::suspend_or_kill()) under the same
/// queue lock this takes. So a post and a timeout racing for one waiter are
/// ordered by that lock: if the post dequeues it first, the timeout finds it
/// gone and the waiter returns Ok with the unit; if the timeout does, the
/// post finds the queue without it and banks the unit for the next wait().
/// A count that went negative for every blocked waiter would need a repair
/// step for the same race, the next post fixing the count after an
/// interrupted wait; this one never counts a waiter, so it has nothing to
/// repair.
///
/// Safe in IRQ context:
///   - every lock here is a spin lock taken with interrupts off, so an
///     interrupt cannot land on this CPU while one of them is held and want
///     it again;
///   - Scheduler::unpark_thread() only puts the woken thread on a run queue
///     and asks for a reschedule (reschedule_mask() records it as pending on
///     this CPU, or sends an IPI to another); it never switches here.
/// Event::signal() has the same shape. semaphore.test (kernel/commands)
/// posts from a timer handler to a parked waiter to check it.
auto Semaphore::post() -> void {
    m_canary.verify<Self>();

    // Interrupts stay off until the unpark below is done, past the guard's
    // unlock(), for the reason Event::signal() gives: the run-queue and PI
    // locks unpark_thread() takes are ones an interrupt on this CPU can want.
    arch::AutoInterruptDisabler const irq_disabler;
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};

    // Nobody queued: bank the unit for the next wait(). lock_for_notify_one()
    // answers none() for an empty queue, and a non-empty list otherwise.
    auto woken = m_wq.lock_for_notify_one<Thread::UnparkList>(Status::Ok);
    if (!woken) {
        m_count += 1;
        return;
    }

    // Committed: the waiter is off the queue with Ok as its status, and no
    // timeout can claim it any more. The wake takes its PI lock, which lies
    // outside the queue lock, so the queue lock goes first.
    guard.unlock();
    Scheduler::unpark_thread(ustl::move(*woken));
}

/// The check and the enqueue happen under one hold of the queue lock, as in
/// Event::wait_etc(): a post that lands before the check is seen by it, and
/// one that lands after finds this thread queued. A gap between the two would
/// let a post bank its unit on a count already checked, after which this
/// thread would queue and sleep with a unit standing -- the lost wake-up
/// Event::wait_etc() once had.
///
/// The PI lock is taken first and held across the park, for the reasons
/// Event::wait_etc() gives: park_thread_postamble() requires it, interrupts
/// and preemption have to be off by then, and the PI lock is the outer one.
auto Semaphore::wait_etc(time::Deadline const deadline, task::Interruptible const interruptible)
    -> Status {
    m_canary.verify<Self>();

    auto const current_thread = Thread::Current::get();
    Guard<SpinLock, IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    {
        Guard<SpinLock, IrqSave> queue_guard{m_wq.get_lock()};
        if (m_count > 0) {
            m_count -= 1;
            return Status::Ok;
        }

        // TimeOut here for a deadline already passed, before anything is
        // queued: that is what makes a wait with a past deadline a poll.
        auto const status = m_wq.park_thread_preamble_locked(
            current_thread, deadline, task::ThreadSignals::None, interruptible);
        if (Status::Ok != status) {
            return status;
        }
    }

    // Ok means a post() handed this thread its unit, which never went
    // through the count. Anything else -- TimeOut, a kill or a suspend --
    // means the thread took itself off the queue and got no unit.
    return m_wq.park_thread_postamble(current_thread, deadline);
}

auto Semaphore::count() -> u64 {
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};
    return m_count;
}

auto Semaphore::num_waiters() -> usize {
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};
    return m_wq.size();
}
} // namespace ours::sync
