#include <ours/task/wait_queue.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>

#include <ours/lockdep.hpp>

namespace ours::task {

    auto WaiterState::sleep(Thread *thread, Interruptible interruptible, Status status) -> void {
        m_blocked_status = status;
        m_interruptible = interruptible;
        Scheduler::Current::park_thread(thread);
    }

    auto WaiterState::unsleep(Thread *thread, Status status) -> void {
        m_blocked_status = status;
        m_interruptible = Interruptible::No;
        Scheduler::unpark_thread(thread);
    }

    WaitQueue::WaitQueue(u32 magic)
        : m_canary(magic)
        , m_waiters()
        , m_lock()
        , m_next_seq(0)
    {}

    auto WaitQueue::park_thread(Thread *thread, time::Deadline deadline, Signals sigmask, Interruptible interruptible) -> Status {
        m_canary.verify<Self>();

        auto const status = park_thread_preamble(thread, deadline, sigmask, interruptible);

        if (Status::Ok != status) {
            return status;
        }

        return park_thread_postamble(thread, deadline);
    }

    auto WaitQueue::park_thread_preamble(Thread *thread, time::Deadline deadline, 
                                         Signals sigmask, Interruptible interruptible) -> Status {
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

    auto WaitQueue::park_thread_postamble(Thread *thread, time::Deadline deadline) -> Status {
        using namespace time;
        DEBUG_ASSERT(Thread::Current::get() == thread);

        auto const use_timer = Deadline::infinite() != deadline;

        Timer timer;
        if (use_timer) {
            timer.activate(deadline, [thread] (Timer *timer) {
                Self::handle_timeout(thread);
                return Timer::HandlerResult::Handled;
            });
        }

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

        Guard guard(m_lock);
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
        auto const thread = Thread::of(&waiter);

        Guard guard(m_lock);

        // Snapshotted here rather than read on every comparison: the tree is
        // walked under m_lock, and the scheduling context is guarded by the
        // thread's PI lock, which the caller already holds.
        waiter.m_queue_key.vdeadline = thread->scheduling_context().deadline();
        waiter.m_queue_key.seq = m_next_seq++;
        waiter.m_waitqueue = this;
        static_cast<void>(m_waiters.insert(waiter));
    }

    auto WaitQueue::dequeue_waiter(WaiterState &waiter, Status status) -> bool {
        Guard guard(m_lock);

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

    auto WaitQueue::wake_dequeued_thread(Thread *thread, Status status) -> void {
        if (auto owq = as<OwnedWaitQueue>(this)) {
            OwnedWaitQueue::begin_propagate(*thread, *owq, OwnedWaitQueue::kRemoveSingleEdgeOp);
        }

        thread->waiter_state().unsleep(thread, status);
    }

    auto WaitQueue::notify_one(Status status) -> void {
        m_canary.verify<Self>();

        Thread *thread = nullptr;
        {
            Guard<sync::SpinLock> guard(m_lock);
            if (m_waiters.empty()) {
                return;
            }

            // begin() is the earliest virtual deadline, i.e. the waiter the
            // scheduler would have run first had none of them blocked.
            auto &waiter = *m_waiters.begin();
            static_cast<void>(m_waiters.erase(m_waiters.iterator_to(waiter)));
            waiter.m_waitqueue = nullptr;
            waiter.m_blocked_status = status;
            thread = Thread::of(&waiter);
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
                Guard<sync::SpinLock> guard(m_lock);
                if (m_waiters.empty()) {
                    return;
                }

                auto &waiter = *m_waiters.begin();
                static_cast<void>(m_waiters.erase(m_waiters.iterator_to(waiter)));
                waiter.m_waitqueue = nullptr;
                waiter.m_blocked_status = status;
                thread = Thread::of(&waiter);
            }

            wake_dequeued_thread(thread, status);
        }
    }

    template <typename UnparkThreads>
    auto WaitQueue::lock_for_notify_one(Status) -> ustl::Option<UnparkThreads> {
        return ustl::none();
    }

    template <typename UnparkThreads>
    auto WaitQueue::lock_for_notify_all(Status) -> ustl::Option<UnparkThreads> {
        return ustl::none();
    }

    template auto WaitQueue::lock_for_notify_one(Status) -> ustl::Option<Thread::UnparkList>;
    template auto WaitQueue::lock_for_notify_all(Status) -> ustl::Option<Thread::UnparkList>;

    auto WaitQueue::handle_timeout(Thread *thread) -> void {
        Guard thread_guard{thread->get_pi_lock()};

        auto const wq = thread->waiter_state().wait_queue();
        DEBUG_ASSERT(wq);

        wq->unpark_thread(thread, Status::TimeOut);
    }
} // namespace ours::task
