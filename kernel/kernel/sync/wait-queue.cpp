#include <ours/sync/wait-queue.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>

#include <ours/lockdep.hpp>

namespace ours::sync {
    using task::Thread;
    using task::Scheduler;

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
        DEBUG_ASSERT(thread->state() == Thread::State::Running);

        auto &waiter = thread->waiter_state();

        dequeue_waiter(waiter, status);
        if (auto owq = as<OwnedWaitQueue>(this)) {
            OwnedWaitQueue::begin_propagate(*thread, *owq, OwnedWaitQueue::kRemoveSingleEdgeOp);
        }

        waiter.unsleep(thread, status);
    }

    auto WaitQueue::update_parked_thread_effective_profile(Thread *thread) -> void {

    }

    auto WaitQueue::enqueue_waiter(WaiterState &waiter) -> void {
        {
            Guard<SpinLock> guard(m_lock);
            m_waiters.push_back(waiter);
        }
        waiter.m_waitqueue = this;
    }

    auto WaitQueue::dequeue_waiter(WaiterState &waiter, Status status) -> void {
        {
            Guard<SpinLock> guard(m_lock);
            m_waiters.erase(m_waiters.iterator_to(waiter));
        }
        waiter.m_waitqueue = 0;
        waiter.m_blocked_status = status;
    }

    auto WaitQueue::notify_one(Status) -> void {
        auto const now = time::SteadyClock::now();
        m_waiters.find(now);
    }

    auto WaitQueue::notify_all(Status) -> void {
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
