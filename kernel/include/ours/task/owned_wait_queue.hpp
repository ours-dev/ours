/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_OWNED_WAIT_QUEUE_HPP
#define OURS_TASK_OWNED_WAIT_QUEUE_HPP 1

#include <ours/task/sched/profiles.hpp>
#include <ours/task/wait_queue.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/traits/is_same.hpp>

namespace ours::task {
template <typename Upstream, typename Downstream>
concept PropagatingStream = requires {
    // From thread to OWQ.
    (ustl::traits::IsSameV<Upstream, Thread> &&
     ustl::traits::IsSameV<Downstream, OwnedWaitQueue>) ||

        // From OWQ to thread.
        (ustl::traits::IsSameV<Upstream, OwnedWaitQueue> &&
         ustl::traits::IsSameV<Downstream, Thread>);
};

class OwnedWaitQueue: protected WaitQueue {
    using Base = WaitQueue;
    using Self = OwnedWaitQueue;

  public:
    enum class NotifyControl {
        None,
    };

    enum class PropagateOp {
        AddSingleEdge,
        RemoveSingleEdge,
        BaseProfileOverrided,
    };

    struct INotifyRequeueHook {
        ustl::function::Fn<auto()->bool> allow;
        ustl::function::Fn<auto()->bool> on_notify;
        ustl::function::Fn<auto()->bool> on_requeue;
    };

    template <PropagateOp>
    struct PropagateTag {};

    constexpr static auto const ADD_SINGLE_EDGE_OP = PropagateTag<PropagateOp::AddSingleEdge>{};

    constexpr static auto const REMOVE_SINGLE_EDGE_OP =
        PropagateTag<PropagateOp::RemoveSingleEdge>{};

    constexpr static auto const BASE_PROFILE_OVERRIDED_OP =
        PropagateTag<PropagateOp::BaseProfileOverrided>{};

    static auto default_wake_hook() -> INotifyRequeueHook &;

    static auto disown_all_queues(Thread *thread) -> void;

    static auto override_thread_base_profile(Thread *thread, sched::BaseProfile const &profile)
        -> void;

    OwnedWaitQueue();

    ~OwnedWaitQueue();

    auto assign_owner(Thread *new_owner) -> Status;

    auto park_and_assign_owner(Thread *const curr, time::Deadline const &, Interruptible) -> Status;

    auto park_and_assign_owner_preamble(Thread *const curr, Thread *new_owner) -> void;
    auto park_and_assign_owner_postamble(Thread *const curr, Thread *new_owner) -> void;

    /// What lock_for_notify_operation() hands back: whether the queue could be
    /// locked, and the waiters it collected.
    ///
    /// A plain aggregate, not an Option, because the caller decomposes it in
    /// the condition of an `if` (P0963R3) and then touches `threads` directly.
    /// An Option would put value() -- and its require_some() -- in front of
    /// every one of those accesses, re-asking a question the condition has
    /// already answered.
    ///
    /// Contextually true only when there is somebody to hand off to: locking
    /// an empty queue succeeds and still leaves nothing to wake, and the two
    /// callers care about the same thing.
    template <typename UnparkThreads> // Thread::UnparkList
    struct NotifyBatch {
        bool locked;
        UnparkThreads threads;

        explicit operator bool() const {
            return locked && !threads.empty();
        }
    };

    template <typename UnparkThreads> // Thread::UnparkList
    auto lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
        -> NotifyBatch<UnparkThreads>;

    template <typename UnparkThreads> // Thread::UnparkList
    auto notify_threads_locked(UnparkThreads threads, INotifyRequeueHook &, NotifyControl) -> void;

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
    static auto lock_pi_chain_common(Applicant *start, Thread *blocking_thread) -> void;

    template <typename Applicant>
    static auto unlock_pi_chain_common(Applicant *start, void *stop_point) -> void;

    static auto snapshot_thread_inherited_profile(Thread *thread) -> sched::InheritedProfile;

    auto park_and_assign_owner_locked(Thread *const curr, time::Deadline const &, Interruptible)
        -> Status;

    auto set_owner(Thread *thread) -> void;

    auto reset_owner() -> void;

    friend class WaitQueue;

    constexpr static auto OWNED_MAGIC = gktl::magic("OwnedWaitQueue");

    Thread *m_owner;
    sched::InheritedProfile m_ipv;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOptions);
    USTL_DECLARE_LIST(Self, List, ManagedOptions);
};

template <>
FORCE_INLINE inline auto WaitQueue::as<OwnedWaitQueue>(Self *self) -> OwnedWaitQueue * {
    DEBUG_ASSERT(self);
    if (self->m_canary.magic() == OwnedWaitQueue::OWNED_MAGIC) {
        // The canary establishes the dynamic type and WaitQueue is the sole base.
        return reinterpret_cast<OwnedWaitQueue *>(self);
    }

    return 0;
}
} // namespace ours::task

#endif // #ifndef OURS_TASK_OWNED_WAIT_QUEUE_HPP
