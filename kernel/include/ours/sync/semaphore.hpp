/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_SEMAPHORE_HPP
#define OURS_SYNC_SEMAPHORE_HPP 1

#include <ours/status.hpp>
#include <ours/task/wait_queue.hpp>
#include <ours/time/timer.hpp>

#include <gktl/canary.hpp>

namespace ours::sync {
/// A counting semaphore: post() hands out one unit, wait() takes one, and a
/// wait blocks while there is none.
///
/// post() is not an Event's signal() under another name. A signal sets a
/// state and is not counted; every post() is kept until a wait() takes it.
/// See Event for which of the two a waiter wants.
///
/// What it is for: a producer that cannot take a sleeping lock -- an
/// interrupt handler queueing a port packet -- tells a consumer "there is one
/// more thing for you" without the two having to agree on what the thing is.
/// The port queues under its own lock, lets the lock go, then posts; a
/// consumer waits first, then takes the queue's lock. A unit is never a
/// claim on a particular packet, only on one turn at the queue.
///
/// The objects:
///   - m_count, the units posted and not yet taken.
///   - m_wq, the threads blocked in wait().
/// Both are guarded by m_wq's spin lock, and together they keep one
/// invariant: **a thread is waiting only while the count is zero**. post()
/// hands its unit straight to the first waiter if there is one, and banks it
/// in the count only if there is none; wait() takes a banked unit if there is
/// one, and queues itself only if there is none.
///
/// The count is never negative: post() hands its unit to the first waiter or
/// increments the count, both under the one IRQ-safe spin lock. The other
/// design lets wait() decrement the count below zero before it blocks, so
/// that post() and wait() can skip the queue lock entirely when no thread has
/// to block (an atomic fast path). That is an optimisation, and this class
/// leaves it out: every post() and wait() takes the queue lock.
/// docs/repairs/261004/kernel-sync.md records it.
class Semaphore {
    using Self = Semaphore;

  public:
    /// Nothing banked. Not explicit, unlike the one below, so that a struct
    /// holding a semaphore can still be built with `{}`.
    Semaphore() : Semaphore(0) {}

    explicit Semaphore(u64 const initial_count) : m_canary(this), m_wq(), m_count(initial_count) {}

    Semaphore(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// Releases the first waiter, or banks one unit for the next wait() if
    /// no thread is waiting.
    ///
    /// May be called in IRQ context. It takes only spin locks, each with
    /// interrupts off, and asks for a reschedule rather than performing one
    /// (Scheduler::reschedule_mask()).
    auto post() -> void;

    /// Takes one unit, blocking for as long as there is none. Not
    /// interruptible: a kill or suspend does not end it.
    auto wait() -> Status {
        return wait_etc(time::Deadline::infinite(), task::Interruptible::No);
    }

    /// Takes one unit, blocking until |deadline| at most. Interruptible: a
    /// kill or a suspend ends the wait with the status the sleep returned.
    ///
    /// TimeOut once the deadline has passed with no unit taken. A deadline
    /// already in the past makes this a poll: it takes a banked unit or
    /// answers TimeOut at once, and never blocks.
    auto wait(time::Deadline const &deadline) -> Status {
        return wait_etc(deadline, task::Interruptible::Yes);
    }

    /// The units banked. For tests and diagnostics only: the answer can be
    /// stale by the time the caller reads it.
    [[nodiscard]]
    auto count() -> u64;

    /// The threads blocked in wait(). For tests and diagnostics only, as
    /// count().
    [[nodiscard]]
    auto num_waiters() -> usize;

  private:
    auto wait_etc(time::Deadline deadline, task::Interruptible interruptible) -> Status;

    gktl::Canary m_canary;
    task::WaitQueue m_wq;
    u64 m_count TA_GUARDED(m_wq.get_lock());
};
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SEMAPHORE_HPP
