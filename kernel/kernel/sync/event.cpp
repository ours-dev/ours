#include <ours/sync/event.hpp>
#include <ours/lockdep.hpp>

#include <ours/time/clock.hpp>

#include <ours/task/scheduler.hpp>

namespace ours::sync {
    using task::Thread;
    using task::Scheduler;

    auto Event::wait_etc(time::Deadline deadline, task::Interruptible interruptible, Signals sigmask) -> Status {
        if (m_result.is_some()) {
            auto const current_thread = Thread::Current::get();
            Guard guard{current_thread->get_pi_lock()};
            m_wq.park_thread(current_thread, deadline, sigmask, interruptible);
        }
        return Status::Ok;
    }

    /// Puts the event back into the unsignalled state, so a later wait()
    /// blocks again. Takes the wait queue lock because signal() decides what
    /// to do by reading m_result under it.
    auto Event::unsignal() -> void {
        Guard guard{m_wq.get_lock()};
        m_result.reset();
    }

    auto Event::signal(Status status) -> void {
        Guard guard{m_wq.get_lock()};

        // We have been signaled.
        if (m_result.is_some()) {
            DEBUG_ASSERT(m_wq.size() == 0);
            return;
        }
        
        if (m_wq.size() == 0) {
            return;
        }

        ustl::Option<Thread::UnparkList> unpark_threads;
        if (!!(m_flags & Flags::AutoUnsignal)) {
            unpark_threads = m_wq.lock_for_notify_one<Thread::UnparkList>(status);
        } else {
            unpark_threads = m_wq.lock_for_notify_all<Thread::UnparkList>(status);
        }

        if (!unpark_threads) {
            return;
        }

        auto const need_unpark = !unpark_threads->empty();
        if (!(m_flags & Flags::AutoUnsignal) || !need_unpark) {
            m_result = status;
        }

        if (need_unpark) {
            guard.unlock();
            Scheduler::unpark_thread(ustl::move(*unpark_threads));
        }
    }
} // namespace ours
