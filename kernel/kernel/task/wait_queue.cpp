#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/task/wait_queue.hpp>
#include <ours/time/clock.hpp>

namespace ours::task {

// Both of these require |thread|'s PI lock, and the requirement cannot be
// written as TA_REQUIRES on their declarations: wait_queue.hpp only has
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

WaitQueue::WaitQueue(Type type)
    // `this` is a WaitQueue*, so the canary takes gktl::magic<WaitQueue>() for
    // every instance, derived ones included. That is deliberate; see Type.
    : m_canary(this), m_type(type), m_waiters(), m_lock(), m_next_seq(0) {}

auto WaitQueue::park_thread(Thread *thread, time::Deadline deadline, ThreadSignals sigmask,
                            Interruptible interruptible) -> Status {
    m_canary.verify<Self>();

    auto const status = park_thread_preamble(thread, deadline, sigmask, interruptible);

    if (Status::Ok != status) {
        return status;
    }

    return park_thread_postamble(thread, deadline);
}

auto WaitQueue::park_thread_preamble(Thread *thread, time::Deadline deadline, ThreadSignals sigmask,
                                     Interruptible interruptible) -> Status {
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

auto WaitQueue::park_thread_preamble_locked(Thread *thread, time::Deadline deadline,
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

auto WaitQueue::park_thread_postamble(Thread *thread, time::Deadline deadline) -> Status {
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

auto WaitQueue::unpark_thread(Thread *thread, Status status) -> void {
    DEBUG_ASSERT(!!(thread->state() & Thread::State::Parked));

    if (!dequeue_waiter(thread->waiter_state(), status)) {
        // Already claimed by a racing notify or timeout; that path owns
        // the wake.
        return;
    }

    wake_dequeued_thread(thread, status);
}

auto WaitQueue::update_parked_thread_effective_profile(Thread *thread) -> void {
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
    static_cast<void>(m_waiters.erase(m_waiters.iterator_to(waiter)));
    // recompute it's effective profile.
    static_cast<void>(m_waiters.insert(waiter));
}

auto WaitQueue::enqueue_waiter(WaiterState &waiter) -> void {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    enqueue_waiter_locked(waiter);
}

auto WaitQueue::enqueue_waiter_locked(WaiterState &waiter) -> void {
    auto const thread = Thread::of(&waiter);

    // Snapshotted here rather than read on every comparison: the tree is
    // walked under m_lock, and the scheduling context is guarded by the
    // thread's PI lock, which the caller already holds.
    waiter.m_queue_key.vdeadline = thread->scheduling_entity().virtual_deadline();
    waiter.m_queue_key.seq = m_next_seq++;
    waiter.m_waitqueue = this;
    static_cast<void>(m_waiters.insert(waiter));
}

auto WaitQueue::dequeue_waiter(WaiterState &waiter, Status status) -> bool {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);

    // A timeout firing on one CPU can meet a notify on another, both aimed
    // at this waiter. Whoever takes the lock second must not erase the
    // node a second time.
    if (waiter.m_waitqueue != this) {
        return false;
    }

    static_cast<void>(m_waiters.erase(m_waiters.iterator_to(waiter)));
    waiter.m_waitqueue = nullptr;
    waiter.m_blocked_status = status;
    return true;
}

/// Removes the head waiter and returns its thread. The queue lock is already
/// held; the four callers differ only in what they do afterwards.
auto WaitQueue::dequeue_front_locked(Status status) -> Thread * {
    auto &waiter = *m_waiters.begin();
    static_cast<void>(m_waiters.erase(m_waiters.iterator_to(waiter)));
    waiter.m_waitqueue = nullptr;
    waiter.m_blocked_status = status;
    return Thread::of(&waiter);
}

auto WaitQueue::wake_dequeued_thread(Thread *thread, Status status) -> void {
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

    if (auto owq = as<OwnedWaitQueue>(this)) {
        OwnedWaitQueue::begin_propagate(*thread, *owq, OwnedWaitQueue::REMOVE_SINGLE_EDGE_OP);
    }

    thread->waiter_state().unsleep(thread, status);
}

auto WaitQueue::notify_one(Status status) -> void {
    m_canary.verify<Self>();

    Thread *thread = nullptr;
    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        if (m_waiters.empty()) {
            return;
        }

        thread = dequeue_front_locked(status);
    }

    // Dequeued under the lock above, so no timeout can still be holding a
    // claim on it; the wake itself runs with the queue lock dropped.
    wake_dequeued_thread(thread, status);
}

auto WaitQueue::notify_all(Status status) -> void {
    m_canary.verify<Self>();

    for (;;) {
        Thread *thread = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard(m_lock);
            if (m_waiters.empty()) {
                return;
            }

            thread = dequeue_front_locked(status);
        }

        wake_dequeued_thread(thread, status);
    }
}

/// Takes waiters off the queue without waking them, so the caller can act in
/// between.
///
/// This is the two-phase form of notify_one()/notify_all(): those dequeue and
/// wake in one step, which is wrong when something has to happen while the
/// waiters are already committed but not yet running. Mutex::release_slowpath
/// publishes the new owner into m_state between the two, and Event::signal
/// records the result before dropping its guard.
///
/// The caller already holds m_lock -- "lock_for" names the phase, not an
/// acquisition. Taking it here would deadlock on the non-recursive SpinLock
/// that Event::signal is holding when it calls in.
///
/// Nothing is returned when the queue is empty, which is what lets the caller
/// distinguish "nobody was waiting" from "somebody was" without inspecting the
/// list.
template <typename UnparkThreads>
auto WaitQueue::lock_for_notify_one(Status status) -> ustl::Option<UnparkThreads> {
    m_canary.verify<Self>();
    m_lock.assert_held();

    if (m_waiters.empty()) {
        return ustl::none();
    }

    // begin() is the earliest virtual deadline -- the waiter the scheduler
    // would have run first had none of them blocked.
    UnparkThreads threads;
    threads.push_back(*dequeue_front_locked(status));
    return ustl::some(ustl::move(threads));
}

template <typename UnparkThreads>
auto WaitQueue::lock_for_notify_all(Status status) -> ustl::Option<UnparkThreads> {
    m_canary.verify<Self>();
    m_lock.assert_held();

    if (m_waiters.empty()) {
        return ustl::none();
    }

    UnparkThreads threads;
    while (!m_waiters.empty()) {
        threads.push_back(*dequeue_front_locked(status));
    }
    return ustl::some(ustl::move(threads));
}

template auto WaitQueue::lock_for_notify_one(Status) -> ustl::Option<Thread::UnparkList>;
template auto WaitQueue::lock_for_notify_all(Status) -> ustl::Option<Thread::UnparkList>;

/// Zircon's WaitQueue::TimeoutHandler().
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
auto WaitQueue::handle_timeout(time::Timer *timer, Thread *thread) -> void {
    auto &pi_lock = thread->get_pi_lock();
    if (timer->trylock_or_cancel(pi_lock)) {
        return;
    }

    auto const wq = thread->waiter_state().wait_queue();
    DEBUG_ASSERT(wq);

    wq->unpark_thread(thread, Status::TimeOut);
    pi_lock.release();
}
} // namespace ours::task
