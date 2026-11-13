/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_WAIT_QUEUE_HPP
#define OURS_TASK_WAIT_QUEUE_HPP 1

#include <ours/task/wait_queue_base.hpp>

#include <ustl/option.hpp>

namespace ours::task {
class WaitQueue final: public WaitQueueBase {
  public:
    WaitQueue() : WaitQueueBase(Type::Plain) {}

    using WaitQueueBase::park_thread;
    using WaitQueueBase::park_thread_postamble;
    using WaitQueueBase::park_thread_preamble_locked;

    auto park_thread_common(Thread *thread, time::Deadline, ThreadSignals) -> Status;

    /// Collect waiters without waking them, so the caller can act while they
    /// are committed but not yet running -- Mutex::release_slowpath publishes
    /// the new owner between the two, Event::signal records its result.
    ///
    /// The queue lock is the *caller's*: "lock_for" names the phase, not an
    /// acquisition. Event::signal already holds it when it calls in, and the
    /// SpinLock is not recursive.
    ///
    /// Nothing is returned for an empty queue, so a caller can tell "nobody
    /// was waiting" from "somebody was" without inspecting the list.
    template <typename UnparkThreads> // Thread::UnparkList
    TA_REQUIRES(get_lock()) auto lock_for_notify_one(Status) -> ustl::Option<UnparkThreads>;

    template <typename UnparkThreads> // Thread::UnparkList
    TA_REQUIRES(get_lock()) auto lock_for_notify_all(Status) -> ustl::Option<UnparkThreads>;

    auto notify_one(Status) -> void;

    auto notify_all(Status) -> void;
};

} // namespace ours::task

#endif // #ifndef OURS_TASK_WAIT_QUEUE_HPP
