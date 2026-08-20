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

#include <ours/task/sched/types.hpp>

#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>
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
            FORCE_INLINE
            auto operator<(QueueKey const &other) const noexcept -> bool {
                if (vdeadline != other.vdeadline) {
                    return vdeadline < other.vdeadline;
                }
                return seq < other.seq;
            }

            task::sched::SchedTime vdeadline;
            u64 seq;
        };

        FORCE_INLINE
        auto queue_key() const noexcept -> QueueKey const & {
            return m_queue_key;
        }
      private:
        friend class WaitQueue;
        friend class OwnedWaitQueue;

        gktl::Canary m_canary;
        WaitQueue *m_waitqueue;
        Status m_blocked_status;
        Interruptible m_interruptible;
                              ustl::collections::intrusive::Compare<WaiterOrder>,
                              WaiterState::ManagedOption);
        Queue m_waiters;
        SpinLock m_lock;
        /// Hands out QueueKey::seq. Per-queue rather than global: it only has
        /// to order waiters against their own queue's peers.
        u64 m_next_seq;
    };

} // namespace ours::task

#endif // #ifndef OURS_TASK_WAIT_QUEUE_HPP
