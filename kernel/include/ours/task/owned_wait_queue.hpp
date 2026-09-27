/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "ustl/option.hpp"
#ifndef OURS_TASK_OWNED_WAIT_QUEUE_HPP
#define OURS_TASK_OWNED_WAIT_QUEUE_HPP 1

#include <ours/task/sched/profiles.hpp>
#include <ours/task/wait_queue.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/function.hpp>
#include <ustl/traits/is_same.hpp>

namespace ours::task {
template <typename Upstream, typename Downstream>
concept PropagatingStream = requires {
    (std::is_same_v<Upstream, Thread> && std::is_same_v<Downstream, OwnedWaitQueue>) ||
        (std::is_same_v<Upstream, OwnedWaitQueue> && std::is_same_v<Downstream, Thread>);
};

class OwnedWaitQueue: protected WaitQueue {
    using Base = WaitQueue;
    using Self = OwnedWaitQueue;

  public:
    enum class NotifyOptions {
        None,
    };

    enum class PropagateOp {
        AddSingleEdge,
        RemoveSingleEdge,
        BaseProfileOverrided,
    };

    /// Asked under the queue's spin lock, so none of the three may allocate:
    /// Function keeps each target inline and refuses at compile time one
    /// larger than 24 bytes.
    struct INotifyRequeueHook {
        ustl::function::Function<auto()->bool> allow;
        ustl::function::Function<auto()->bool> on_notify;
        ustl::function::Function<auto()->bool> on_requeue;
    };

    template <PropagateOp>
    struct PropagateTag {};

    constexpr static auto ADD_SINGLE_EDGE_OP = PropagateTag<PropagateOp::AddSingleEdge>{};

    constexpr static auto REMOVE_SINGLE_EDGE_OP = PropagateTag<PropagateOp::RemoveSingleEdge>{};

    constexpr static auto BASE_PROFILE_OVERRIDED_OP =
        PropagateTag<PropagateOp::BaseProfileOverrided>{};

    static auto default_wake_hook() -> INotifyRequeueHook &;

    static auto disown_all_queues(Thread *thread) -> void;

    static auto override_thread_base_profile(Thread *thread, sched::BaseProfile const &profile)
        -> void;

    OwnedWaitQueue();

    ~OwnedWaitQueue();

    auto assign_owner(Thread *new_owner) -> Status;

    auto park_and_assign_owner(Thread *const, time::Deadline const &, Interruptible) -> Status;

    /// The two halves of park_and_assign_owner(), for a caller whose reason
    /// to block lives outside the queue and has to be re-read under the
    /// queue's lock -- Mutex decides between taking the mutex and queueing
    /// for it by looking at its state word there.
    ///
    /// park_and_assign_owner() cannot serve such a caller: it enqueues under
    /// a lock it takes and drops by itself, so whatever the caller checked
    /// beforehand can change before the enqueue, and a waker that runs in that
    /// gap sees an empty queue. Same contract as
    /// WaitQueue::park_thread_preamble_locked()/park_thread_postamble():
    /// |curr|'s PI lock is held, interrupts and preemption off, across both
    /// halves; the queue lock across the preamble only.
    TA_REQUIRES(get_lock())
    auto park_and_assign_owner_preamble_locked(Thread *const, time::Deadline const &, Interruptible)
        -> Status;

    auto park_and_assign_owner_postamble(Thread *const, time::Deadline const &) -> Status;

    struct ParkAndAssignOwner {
        Thread *new_owner;
    };
    auto lock_for_park_and_assign_owner_or_backoff(Thread *const, Thread *new_owner)
        -> ustl::Option<ParkAndAssignOwner>;

    /// First half of a wake: takes up to |max_notify| waiters off the queue
    /// without waking them. The caller holds get_lock() -- "lock_for" names
    /// the phase, as it does for WaitQueue::lock_for_notify_one() -- so that
    /// whatever it publishes next (Mutex::release_slowpath() writes the new
    /// owner into its state word) is ordered against every waiter that checks
    /// that word under the same lock before enqueueing.
    ///
    /// Nothing is returned for an empty queue.
    template <typename UnparkThreads> // Thread::UnparkList
    TA_REQUIRES(get_lock()) auto lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
        -> ustl::Option<UnparkThreads>;

    /// Second half: wakes what lock_for_notify_operation() collected.
    ///
    /// Called with the queue lock *dropped*, which is where this parts ways
    /// with Zircon's OwnedWaitQueue::WakeThreadsLocked(). Waking takes each
    /// thread's PI lock, and in this tree the PI lock is the outer one and
    /// the queue lock the inner (see WaitQueue::enqueue_waiter); Zircon can
    /// wake under the queue lock only because its ChainLock transaction
    /// already holds every lock involved. Hence no "_locked" suffix.
    template <typename UnparkThreads> // Thread::UnparkList
    auto notify_threads(UnparkThreads threads, INotifyRequeueHook &, NotifyOptions) -> void;

    using Base::get_lock;
    using Base::is_empty;
    using Base::peek;

  private:
    template <PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    static auto begin_propagate(Upstream &, Downstream &, PropagateTag<Op>) -> void;

    template <PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    static auto complete_propagate(Upstream &, Downstream &, sched::InheritedProfile const *,
                                   sched::InheritedProfile const *, PropagateTag<Op>) -> void;

    static auto lock_for_owner_replacement(Thread *new_owner, Thread *blocking_thread) -> void;

    template <typename Applicant>
    static auto lock_pi_chain_common(Applicant *start) -> void;

    template <typename Applicant>
    static auto unlock_pi_chain_common(Applicant *start, void *stop_point) -> void;

    static auto snapshot_thread_inherited_profile(Thread *thread) -> sched::InheritedProfile;

    auto park_and_assign_owner_locked(Thread *const curr, time::Deadline const &, Interruptible)
        -> Status;

    auto set_owner(Thread *thread) -> void;

    auto reset_owner() -> void;

    friend class WaitQueue;

    /// "Is this really a live OwnedWaitQueue?" is two questions here, not one.
    /// The canary holds WaitQueue's magic on every instance -- see the note on
    /// WaitQueue::Type for why it must -- so on its own it says only that the
    /// storage is still a WaitQueue. Which kind it is, m_type answers.
    FORCE_INLINE
    auto verify_owned() const -> void {
        m_canary.verify<Base>();
        DEBUG_ASSERT(Type::Owned == m_type, "an OwnedWaitQueue method on a plain WaitQueue");
    }

    Thread *m_owner;
    sched::InheritedProfile m_ipv;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOptions);
    USTL_DECLARE_LIST(Self, List, ManagedOptions);
};

template <>
FORCE_INLINE
inline auto WaitQueue::as<OwnedWaitQueue>(Self *self) -> OwnedWaitQueue * {
    DEBUG_ASSERT(self);
    if (Type::Owned == self->m_type) {
        // m_type establishes the dynamic type and WaitQueue is the sole base.
        return reinterpret_cast<OwnedWaitQueue *>(self);
    }

    return 0;
}
} // namespace ours::task

#endif // #ifndef OURS_TASK_OWNED_WAIT_QUEUE_HPP
