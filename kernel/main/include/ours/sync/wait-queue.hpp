/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_WAIT_QUEUE_HPP
#define OURS_TASK_WAIT_QUEUE_HPP 1

#include <ours/sync/types.hpp>
#include <ours/sync/spin-lock.hpp>

#include <ours/task/types.hpp>
#include <ours/time/timer.hpp>

#include <ours/types.hpp>
#include <ours/status.hpp>
#include <ours/signals.hpp>

#include <ustl/collections/intrusive/set.hpp>
#include <ustl/option.hpp>

#include <gktl/canary.hpp>

namespace ours::sync {
    class WaiterState {
        using Self = WaiterState;
      public:
        auto sleep(task::Thread *, Interruptible, Status) -> void;

        auto unsleep(task::Thread *, Status status) -> void;

        auto wait_queue() -> WaitQueue * {
          return m_waitqueue;
        }

        auto is_interruptible() const -> bool {
            return m_interruptible == Interruptible::Yes;
        }

        FORCE_INLINE
        auto status() const -> Status {
            return m_blocked_status;
        }
      private:
        friend class WaitQueue;
        friend class OwnedWaitQueue;

        gktl::Canary m_canary;
        WaitQueue *m_waitqueue;
        Status m_blocked_status;
        Interruptible m_interruptible;
    };

    class WaitQueue {
        using Self = WaitQueue;
      public:
        template <typename Derived>
        static auto as(Self *self) -> Derived *;

        WaitQueue() = default;

        ~WaitQueue() = default;

        /// Puts the current thread at the end of waiter queue and then blocks
        /// until some threads wakes the queue up again.
        auto park_thread(task::Thread *, time::Deadline = time::Deadline::infinite(),
                         Signals = Signals::None, Interruptible = Interruptible::Yes) -> Status;

        auto park_thread_common(task::Thread *thread, time::Deadline, Signals) -> Status;

        auto unpark_thread(task::Thread *thread, Status = Status::Ok) -> void;

        auto update_parked_thread_effective_profile(task::Thread *thread) -> void;

        template <typename UnparkThreads> // Thread::UnparkList
        auto lock_for_notify_one(Status) -> ustl::Option<UnparkThreads>;

        template <typename UnparkThreads> // Thread::UnparkList
        auto lock_for_notify_all(Status) -> ustl::Option<UnparkThreads>;

        auto notify_one(Status) -> void;

        auto notify_all(Status) -> void;

        FORCE_INLINE
        auto size() const -> usize { return m_waiters.size(); }

        FORCE_INLINE
        auto is_empty() const -> bool { return m_waiters.empty(); }

        FORCE_INLINE
        auto peek() -> task::Thread * { return 0; }

        FORCE_INLINE
        auto get_lock() -> SpinLock & { return m_lock; }
      protected:
        WaitQueue(u32 magic);

        auto park_thread_preamble(task::Thread *, time::Deadline, Signals, Interruptible) -> Status;

        auto park_thread_postamble(task::Thread *, time::Deadline) -> Status;

        auto enqueue_waiter(WaiterState &waiter) -> void;

        auto dequeue_waiter(WaiterState &waiter, Status status) -> void;

        /// Handler for time counted tasks.
        static auto handle_timeout(task::Thread *thread) -> void;

        static constexpr auto MAGIC = gktl::magic("WaitQueue");

        gktl::Canary m_canary;
        USTL_DECLARE_MULTISET(WaiterState, Queue, WaiterState::ManagedOption);
        Queue m_waiters;
        SpinLock m_lock;
    };

} // namespace ours::task

#endif // #ifndef OURS_TASK_WAIT_QUEUE_HPP