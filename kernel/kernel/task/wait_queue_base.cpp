/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/task/wait_queue_base.hpp>
#include <ours/time/clock.hpp>

namespace ours::task {

// Both of these require |thread|'s PI lock, and the requirement cannot be
// written as TA_REQUIRES on their declarations: wait_queue_base.hpp only has
// Thread forward-declared (thread.hpp includes it, not the other way round),
// and a capability expression needs the complete type. assert_held() is the
// substitute -- it is TA_ASSERT, so from that line on the analysis treats
// the lock as held and checks the park/unpark calls beneath it, and at run
// time it is a real check of the same precondition. owned_wait_queue.cpp
// uses the same pattern for the same reason.
auto WaiterState::sleep(Thread *thread, Interruptible interruptible, Status status) -> void {
    thread->get_pi_lock().assert_held();
    m_blocked_status = status;
    m_interruptible = interruptible;
    Scheduler::Current::park_thread(thread);
}

auto WaiterState::unsleep(Thread *thread, Status status) -> void {
    thread->get_pi_lock().assert_held();
    m_blocked_status = status;
    m_interruptible = Interruptible::No;
    Scheduler::unpark_thread(thread);
}

WaitQueueBase::WaitQueueBase(Type const type)
    : m_canary(this), m_type(type), m_waiters(), m_lock(), m_next_seq(0) {}

auto WaitQueueBase::verify_type([[maybe_unused]] Type const type) const -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(m_type == type);
}

auto WaitQueueBase::as_owned(Self *const queue) -> OwnedWaitQueue * {
    if (queue == nullptr || queue->m_type != Type::Owned) {
        return nullptr;
    }
    return static_cast<OwnedWaitQueue *>(queue);
}

auto WaitQueueBase::park_thread(Thread *thread, time::Deadline deadline, ThreadSignals sigmask,
                                Interruptible interruptible) -> Status {
    m_canary.verify<Self>();

    auto const status = park_thread_preamble(thread, deadline, sigmask, interruptible);

    if (Status::Ok != status) {
        return status;
    }

    return park_thread_postamble(thread, deadline);
}

auto WaitQueueBase::park_thread_preamble(Thread *thread, time::Deadline deadline,
                                         ThreadSignals sigmask, Interruptible interruptible)
    -> Status {
    using namespace time;
    DEBUG_ASSERT(Thread::Current::get() == thread);

    if (deadline.when() < SteadyClock::now()) {
        return Status::TimeOut;
    }

    if (Interruptible::Yes == interruptible && !!(thread->signals() & ~sigmask)) {
        auto const status = thread->check_kill_or_suspend_signal();
        if (Status::Ok != status) {
            return status;
        }
    }

    thread->set_state_parked();

    auto &waiter_state = thread->waiter_state();
    waiter_state.m_waitqueue = this;
    waiter_state.m_interruptible = interruptible;

    enqueue_waiter(waiter_state);
    return Status::Ok;
}

auto WaitQueueBase::park_thread_preamble_locked(Thread *thread, time::Deadline deadline,
                                                ThreadSignals sigmask, Interruptible interruptible)
    -> Status {
    using namespace time;
    DEBUG_ASSERT(Thread::Current::get() == thread);

    if (deadline.when() < SteadyClock::now()) {
        return Status::TimeOut;
    }

    if (Interruptible::Yes == interruptible && !!(thread->signals() & ~sigmask)) {
        auto const status = thread->check_kill_or_suspend_signal();
        if (Status::Ok != status) {
            return status;
        }
    }

    thread->set_state_parked();

    auto &waiter_state = thread->waiter_state();
    waiter_state.m_waitqueue = this;
    waiter_state.m_interruptible = interruptible;

    enqueue_waiter_locked(waiter_state);
    return Status::Ok;
}

auto WaitQueueBase::park_thread_postamble(Thread *thread, time::Deadline deadline) -> Status {
    using namespace time;
    DEBUG_ASSERT(Thread::Current::get() == thread);

    auto const use_timer = Deadline::infinite() != deadline;

    Timer timer;
    if (use_timer) {
        timer.activate(deadline, [thread](Timer *timer) {
            Self::handle_timeout(timer, thread);
            return Timer::HandlerResult::Handled;
        });
    }

    // See WaiterState::sleep() for why this is an assert and not TA_REQUIRES.
    thread->get_pi_lock().assert_held();
    Scheduler::Current::park_thread(thread);

    // If we reach at here before timeout, cancel the timer to prevent error.
    if (use_timer) {
        timer.cancel();
    }

    auto &waiter_state = thread->waiter_state();
    waiter_state.m_interruptible = Interruptible::No;
    return waiter_state.status();
}

auto WaitQueueBase::unpark_thread(Thread *thread, Status status) -> void {
    // Both callers come here holding |thread|'s PI lock, and this used to
    // end in wake_dequeued_thread(), which takes that lock again. SpinLock
    // does not nest: in dbg_1 that is SpinLock::acquire()'s !is_held()
    // assertion, and without assertions the CPU spins for good, interrupts
    // off, on a lock it holds itself. See WaiterState::sleep() for why this
    // is an assert and not TA_REQUIRES.
    thread->get_pi_lock().assert_held();
    DEBUG_ASSERT(!!(thread->state() & Thread::State::Parked));

    if (!dequeue_waiter(thread->waiter_state(), status)) {
        // Already claimed by a racing notify or timeout; that path owns
        // the wake.
        return;
    }

    wake_dequeued_thread_locked(thread, status);
}

auto WaitQueueBase::update_parked_thread_effective_profile(Thread *thread) -> void {
    m_canary.verify<Self>();

    auto &waiter = thread->waiter_state();

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    if (waiter.m_waitqueue != this) {
        return;
    }

    // The key is part of the tree's ordering invariant, so the node has to
    // come out before it changes and go back in afterwards. The arrival
    // sequence is deliberately left alone: a waiter re-keyed by PI should
    // not lose the tiebreak it earned by getting here first.
    auto _ = m_waiters.erase(m_waiters.iterator_to(waiter));
    // recompute it's effective profile.
    auto _ = m_waiters.insert(waiter);
}

auto WaitQueueBase::enqueue_waiter(WaiterState &waiter) -> void {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    enqueue_waiter_locked(waiter);
}

auto WaitQueueBase::enqueue_waiter_locked(WaiterState &waiter) -> void {
    auto const thread = Thread::of(&waiter);

    // Snapshotted here rather than read on every comparison: the tree is
    // walked under m_lock, and the scheduling context is guarded by the
    // thread's PI lock, which the caller already holds.
    waiter.m_queue_key.vdeadline = thread->scheduler_state().virtual_deadline();
    waiter.m_queue_key.seq = m_next_seq++;
    waiter.m_waitqueue = this;
    auto _ = m_waiters.insert(waiter);
}

auto WaitQueueBase::dequeue_waiter(WaiterState &waiter, Status status) -> bool {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);

    // A timeout firing on one CPU can meet a notify on another, both aimed
    // at this waiter. Whoever takes the lock second must not erase the
    // node a second time.
    if (waiter.m_waitqueue != this) {
        return false;
    }

    auto _ = m_waiters.erase(m_waiters.iterator_to(waiter));
    waiter.m_waitqueue = nullptr;
    waiter.m_blocked_status = status;
    return true;
}

/// Removes the head waiter and returns its thread. The queue lock is already
/// held; the four callers differ only in what they do afterwards.
auto WaitQueueBase::dequeue_front_locked(Status status) -> Thread * {
    auto &waiter = *m_waiters.begin();
    auto _ = m_waiters.erase(m_waiters.iterator_to(waiter));
    waiter.m_waitqueue = nullptr;
    waiter.m_blocked_status = status;
    return Thread::of(&waiter);
}

auto WaitQueueBase::wake_dequeued_thread(Thread *thread, Status status) -> void {
    // The waker takes the woken thread's PI lock: unsleep() ends in
    // Scheduler::unpark_thread(), which requires it (it changes the thread's
    // state and queues it), and the PI propagation above it reads the
    // thread's scheduling context, which that lock guards. Until this Guard
    // existed the wake ran with no lock on |thread| at all, racing a
    // suspend_or_kill() that reads state() under the same lock.
    //
    // Lock order: PI lock outside, queue lock inside -- the queue lock was
    // dropped by the caller before it got here, so nothing is inverted.
    // Preemption is disabled as well as interrupts, as every other holder of
    // a PI lock around the unpark does (Thread::resume()): the thread being
    // woken must not preempt us while we still hold its lock.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> guard{thread->get_pi_lock()};
    wake_dequeued_thread_locked(thread, status);
}

auto WaitQueueBase::wake_dequeued_thread_locked(Thread *thread, Status status) -> void {
    thread->get_pi_lock().assert_held();

    if (auto *const owq = as_owned(this)) {
        OwnedWaitQueue::begin_propagate(*thread, *owq, OwnedWaitQueue::REMOVE_SINGLE_EDGE_OP);
    }

    thread->waiter_state().unsleep(thread, status);
}

/// Wakes a waiter whose deadline has passed, with Status::TimeOut.
///
/// The thread that armed this timer parks with its PI lock held
/// (Event::wait_etc() takes it around park_thread()) and, once woken for
/// any other reason, calls Timer::cancel() still holding it. cancel() then
/// spins until this handler has returned. If this handler simply took the
/// PI lock it would spin on the parked thread, which spins on us: deadlock.
/// So the lock is taken with trylock_or_cancel(), which gives up the moment
/// cancel() has marked the timer -- there is nothing left to time out, the
/// thread is already awake.
///
/// The lock is released by hand because lockdep::Guard cannot adopt a lock
/// that was acquired by a try. Interrupts are off already (timer handlers
/// run from the tick), which is the state Guard without IrqSave assumes.
auto WaitQueueBase::handle_timeout(time::Timer *timer, Thread *thread) -> void {
    auto &pi_lock = thread->get_pi_lock();
    if (timer->trylock_or_cancel(pi_lock)) {
        return;
    }

    // No queue: a notify dequeued the thread and is now waiting for this PI
    // lock to unpark it (Event::signal() takes it only after dropping the
    // queue lock). That wake is the thread's; there is nothing to time out.
    auto const wq = thread->waiter_state().wait_queue();
    if (wq != nullptr) {
        wq->unpark_thread(thread, Status::TimeOut);
    }
    pi_lock.release();
}
} // namespace ours::task
