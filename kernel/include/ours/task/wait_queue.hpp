/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_WAIT_QUEUE_HPP
#define OURS_TASK_WAIT_QUEUE_HPP 1

#include <gktl/canary.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/sync/types.hpp>
#include <ours/task/sched/types.hpp>
#include <ours/task/types.hpp>
#include <ours/time/timer.hpp>
#include <ours/types.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>
#include <ustl/option.hpp>

namespace ours::task {
class WaiterState {
    using Self = WaiterState;

  public:
    auto sleep(Thread *, Interruptible, Status) -> void;

    auto unsleep(Thread *, Status status) -> void;

    auto wait_queue() -> WaitQueue * {
        return m_waitqueue;
    }

    auto is_interruptible() const -> bool {
        return m_interruptible == Interruptible::Yes;
    }

    FORCE_INLINE auto status() const -> Status {
        return m_blocked_status;
    }

    /// The order a waiter takes its turn in.
    ///
    /// Priority and waiting time both have to count here, and the run
    /// queue already has a number that folds them together: the EEVDF
    /// virtual deadline. A heavier weight pulls the deadline in, and a
    /// thread that has been owed service long enough builds positive lag,
    /// which pulls it in as well. So the wait queue sorts on that same
    /// number rather than inventing a second, unrelated priority policy --
    /// which also means PI propagation moves waiters in units the
    /// scheduler already agrees with.
    ///
    /// See Stoica & Abdel-Wahab, "Earliest Eligible Virtual Deadline
    /// First: A Flexible and Accurate Mechanism for Proportional Share
    /// Resource Allocation", ODU TR-95-22 (1995).
    ///
    /// The deadline is snapshotted at enqueue instead of read live. A
    /// thread's scheduling context is guarded by its own PI lock, and that
    /// lock must never be taken underneath a wait queue's spin lock. A
    /// waiter whose profile changes while it is parked gets re-keyed
    /// explicitly, through
    /// WaitQueue::update_parked_thread_effective_profile.
    struct QueueKey {
        /// Equal deadlines fall back to arrival order, so waiters the
        /// scheduler considers interchangeable are still served FIFO
        /// rather than in whatever order the tree happens to hold them.
        FORCE_INLINE auto operator<(QueueKey const &other) const noexcept -> bool {
            if (vdeadline != other.vdeadline) {
                return vdeadline < other.vdeadline;
            }
            return seq < other.seq;
        }

        sched::SchedTime vdeadline;
        u64 seq;
    };

    FORCE_INLINE auto queue_key() const noexcept -> QueueKey const & {
        return m_queue_key;
    }

  private:
    friend class WaitQueue;
    friend class OwnedWaitQueue;

    gktl::Canary m_canary;
    WaitQueue *m_waitqueue;
    Status m_blocked_status;
    Interruptible m_interruptible;
    QueueKey m_queue_key;
    ustl::collections::intrusive::SetMemberHook<> m_managed_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);
};

/// noexcept is load-bearing: the intrusive tree static_asserts that its
/// comparator cannot throw.
using WaiterOrder = decltype([](WaiterState const &lhs, WaiterState const &rhs) noexcept {
    return lhs.queue_key() < rhs.queue_key();
});

class WaitQueue {
    using Self = WaitQueue;

  public:
    template <typename Derived>
    static auto as(Self *self) -> Derived *;

    WaitQueue() = default;

    ~WaitQueue() = default;

    /// Puts the current thread at the end of waiter queue and then blocks
    /// until some threads wakes the queue up again.
    auto park_thread(Thread *, time::Deadline = time::Deadline::infinite(), Signals = Signals::None,
                     Interruptible = Interruptible::Yes) -> Status;

    auto park_thread_common(Thread *thread, time::Deadline, Signals) -> Status;

    auto unpark_thread(Thread *thread, Status = Status::Ok) -> void;

    auto update_parked_thread_effective_profile(Thread *thread) -> void;

    template <typename UnparkThreads> // Thread::UnparkList
    auto lock_for_notify_one(Status) -> ustl::Option<UnparkThreads>;

    template <typename UnparkThreads> // Thread::UnparkList
    auto lock_for_notify_all(Status) -> ustl::Option<UnparkThreads>;

    auto notify_one(Status) -> void;

    auto notify_all(Status) -> void;

    FORCE_INLINE auto size() const -> usize {
        return m_waiters.size();
    }

    FORCE_INLINE auto is_empty() const -> bool {
        return m_waiters.empty();
    }

    FORCE_INLINE auto peek() -> Thread * {
        return 0;
    }

    FORCE_INLINE auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  protected:
    WaitQueue(u32 magic);

    auto park_thread_preamble(Thread *, time::Deadline, Signals, Interruptible) -> Status;

    auto park_thread_postamble(Thread *, time::Deadline) -> Status;

    /// Both of these expect the caller to hold |waiter|'s thread PI lock.
    /// The queue's own spin lock is taken underneath it, never the other
    /// way round.
    auto enqueue_waiter(WaiterState &waiter) -> void;

    /// Returns false if the waiter was already off this queue, which is
    /// the normal outcome when a timeout and a notify race for it.
    auto dequeue_waiter(WaiterState &waiter, Status status) -> bool;

    /// Second half of a wake: propagates the lost PI edge and resumes the
    /// thread. Must be called with the queue lock dropped, and only for a
    /// thread this queue actually dequeued.
    auto wake_dequeued_thread(Thread *thread, Status status) -> void;

    /// Handler for time counted tasks.
    static auto handle_timeout(Thread *thread) -> void;

    static constexpr auto MAGIC = gktl::magic("WaitQueue");

    gktl::Canary m_canary;
    USTL_DECLARE_MULTISET(WaiterState, Queue, ustl::collections::intrusive::Compare<WaiterOrder>,
                          WaiterState::ManagedOption);
    Queue m_waiters;
    sync::SpinLock m_lock;
    /// Hands out QueueKey::seq. Per-queue rather than global: it only has
    /// to order waiters against their own queue's peers.
    u64 m_next_seq;
};

} // namespace ours::task

#endif // #ifndef OURS_TASK_WAIT_QUEUE_HPP
