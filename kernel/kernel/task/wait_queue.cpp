#include <ours/lockdep.hpp>
#include <ours/task/thread.hpp>
#include <ours/task/wait_queue.hpp>

namespace ours::task {

auto WaitQueue::notify_one(Status status) -> void {
    verify_type(Type::Plain);

    Thread *thread = nullptr;
    {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};
        if (is_empty()) {
            return;
        }

        thread = dequeue_front_locked(status);
    }

    // Dequeued under the lock above, so no timeout can still be holding a
    // claim on it; the wake itself runs with the queue lock dropped.
    wake_dequeued_thread(thread, status);
}

auto WaitQueue::notify_all(Status status) -> void {
    verify_type(Type::Plain);

    for (;;) {
        Thread *thread = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard{get_lock()};
            if (is_empty()) {
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
    verify_type(Type::Plain);
    get_lock().assert_held();

    if (is_empty()) {
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
    verify_type(Type::Plain);
    get_lock().assert_held();

    if (is_empty()) {
        return ustl::none();
    }

    UnparkThreads threads;
    while (!is_empty()) {
        threads.push_back(*dequeue_front_locked(status));
    }
    return ustl::some(ustl::move(threads));
}

template auto WaitQueue::lock_for_notify_one(Status) -> ustl::Option<Thread::UnparkList>;
template auto WaitQueue::lock_for_notify_all(Status) -> ustl::Option<Thread::UnparkList>;

} // namespace ours::task
