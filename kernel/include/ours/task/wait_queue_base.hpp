/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_WAIT_QUEUE_BASE_HPP
#define OURS_TASK_WAIT_QUEUE_BASE_HPP 1

#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/sync/types.hpp>
#include <ours/task/scheduler_types.hpp>
#include <ours/task/types.hpp>
#include <ours/time/timer.hpp>
#include <ours/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>

namespace ours::task {
class WaiterState {
    using Self = WaiterState;

  public:
    auto sleep(Thread *, Interruptible, Status) -> void;

    auto unsleep(Thread *, Status status) -> void;

    auto wait_queue() -> WaitQueueBase * {
        return m_waitqueue;
    }

    [[nodiscard]]
    auto is_interruptible() const -> bool {
        return m_interruptible == Interruptible::Yes;
    }

    FORCE_INLINE
    auto status() const -> Status {
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
    /// WaitQueueBase::update_parked_thread_effective_profile.
    struct QueueKey {
        /// Equal deadlines fall back to arrival order, so waiters the
        /// scheduler considers interchangeable are still served FIFO
        /// rather than in whatever order the tree happens to hold them.
        FORCE_INLINE
        auto operator<(QueueKey const &other) const noexcept -> bool {
            if (vdeadline != other.vdeadline) {
                return vdeadline < other.vdeadline;
            }
            return seq < other.seq;
        }

        SchedTime vdeadline;
        u64 seq;
    };

    [[nodiscard]]
    FORCE_INLINE
    auto queue_key() const noexcept -> QueueKey const & {
        return m_queue_key;
    }

  private:
    friend class WaitQueueBase;

    gktl::Canary m_canary;
    WaitQueueBase *m_waitqueue;
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

/// Shared wait queue storage and operations for both concrete queue types.
class WaitQueueBase {
    using Self = WaitQueueBase;

  public:
    /// Wakes |thread| out of this queue, if it is still waiting in it.
    ///
    /// The caller holds |thread|'s PI lock, and keeps it: this is the wake
    /// aimed at one particular thread, and the only reason to aim at one is
    /// that the caller has just looked at it under that lock and decided --
    /// suspend_or_kill() saw it Parked and interruptible, handle_timeout()
    /// won the lock against the thread's own timer cancel.
    auto unpark_thread(Thread *thread, Status = Status::Ok) -> void;

    auto update_parked_thread_effective_profile(Thread *thread) -> void;

    FORCE_INLINE
    auto size() const -> usize {
        return m_waiters.size();
    }

    FORCE_INLINE
    auto is_empty() const -> bool {
        return m_waiters.empty();
    }

    FORCE_INLINE
    auto peek() -> Thread * {
        return nullptr;
    }

    /// TA_RETURN_CAPABILITY is what lets `TA_REQUIRES(get_lock())` on a method
    /// be satisfied by a `Guard<sync::SpinLock, IrqSave> guard{m_lock}` in its
    /// caller: the analysis matches capabilities by expression, and without
    /// this a call to get_lock() is an opaque value unrelated to m_lock.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  protected:
    enum class Type : u32 {
        Plain,
        Owned,
    };

    ~WaitQueueBase() = default;

    /// Parks the current thread until a wake or the deadline.
    auto park_thread(Thread *, time::Deadline = time::Deadline::infinite(),
                     ThreadSignals = ThreadSignals::None, Interruptible = Interruptible::Yes)
        -> Status;

    /// The two halves of park_thread(), for a waiter that must look at the
    /// condition it is waiting for *under this queue's lock*.
    ///
    /// park_thread() takes the queue lock only to enqueue. A waiter that
    /// checks its condition under that lock, releases it, and then calls
    /// park_thread() leaves a gap in which a notify can find the condition
    /// false-to-true, see an empty queue, and wake nobody -- after which the
    /// waiter enqueues and sleeps for good. sync::Event::wait_etc() did
    /// exactly that. The fix is to enqueue before the lock is let go:
    ///
    ///     Guard<sync::SpinLock, IrqSave> guard{queue.get_lock()};
    ///     if (condition) { ...return; }
    ///     status = queue.park_thread_preamble_locked(...);   // enqueued
    ///     guard.unlock();
    ///     status = queue.park_thread_postamble(...);         // parks
    ///
    /// A notify that comes after the unlock finds the waiter queued; the wake
    /// it then issues waits on the waiter's PI lock, which the waiter holds
    /// until it has actually parked, so nothing is lost there either.
    TA_REQUIRES(get_lock())
    auto park_thread_preamble_locked(Thread *, time::Deadline, ThreadSignals, Interruptible)
        -> Status;

    auto park_thread_postamble(Thread *, time::Deadline) -> Status;

    /// Removes the head waiter and returns its thread. notify_one/notify_all
    /// and the two-phase lock_for_notify_* pair differ only in what they do
    /// after this.
    TA_REQUIRES(get_lock())
    auto dequeue_front_locked(Status status) -> Thread *;

    /// Second half of a wake: propagates the lost PI edge and resumes the
    /// thread. Must be called with the queue lock dropped, and only for a
    /// thread this queue actually dequeued. Takes |thread|'s PI lock itself,
    /// which is why the queue lock has to be gone first (PI lock is the outer
    /// one).
    auto wake_dequeued_thread(Thread *thread, Status status) -> void;

    auto verify_type(Type type) const -> void;

    static auto as_owned(Self *queue) -> OwnedWaitQueue *;

  private:
    friend class WaitQueue;
    friend class OwnedWaitQueue;

    explicit WaitQueueBase(Type type);

    auto park_thread_preamble(Thread *, time::Deadline, ThreadSignals, Interruptible) -> Status;

    /// Both of these expect the caller to hold |waiter|'s thread PI lock.
    /// The queue's own spin lock is taken underneath it, never the other
    /// way round.
    auto enqueue_waiter(WaiterState &waiter) -> void;

    /// enqueue_waiter() for a caller already holding the queue lock.
    TA_REQUIRES(get_lock())
    auto enqueue_waiter_locked(WaiterState &waiter) -> void;

    /// Returns false if the waiter was already off this queue, which is
    /// the normal outcome when a timeout and a notify race for it.
    auto dequeue_waiter(WaiterState &waiter, Status status) -> bool;

    /// wake_dequeued_thread() for a caller that already holds |thread|'s PI
    /// lock: unpark_thread(). SpinLock does not nest, so that caller must not
    /// go through the form that takes the lock. The caller's hold has to give
    /// what wake_dequeued_thread()'s own does, interrupts and preemption off:
    /// suspend_or_kill() takes it IrqSaveAndPreemptDisable, and
    /// handle_timeout() runs in the timer interrupt.
    auto wake_dequeued_thread_locked(Thread *thread, Status status) -> void;

    /// Handler for time counted tasks.
    static auto handle_timeout(time::Timer *timer, Thread *thread) -> void;

    /// The canary checks the base storage. The immutable tag identifies the
    /// concrete queue for owner-specific operations.
    gktl::Canary m_canary;
    Type const m_type;
    USTL_DECLARE_MULTISET(WaiterState, Queue, ustl::collections::intrusive::Compare<WaiterOrder>,
                          WaiterState::ManagedOption);
    Queue m_waiters;
    /// Held with interrupts off, always: a timeout firing on this CPU takes it
    /// from its timer callback (handle_timeout(), through dequeue_waiter()) to
    /// take its waiter off the queue, and would spin for ever on a holder it
    /// had interrupted.
    sync::SpinLock m_lock;
    /// Hands out QueueKey::seq. Per-queue rather than global: it only has
    /// to order waiters against their own queue's peers.
    u64 m_next_seq;
};

} // namespace ours::task

#endif // #ifndef OURS_TASK_WAIT_QUEUE_BASE_HPP
