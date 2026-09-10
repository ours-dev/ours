/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "ustl/option.hpp"
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
    (std::is_same_v<Upstream, Thread> && std::is_same_v<Downstream, OwnedWaitQueue>) ||
        (std::is_same_v<Upstream, OwnedWaitQueue> && std::is_same_v<Downstream, Thread>);
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

    struct ParkAndAssignOwner {
        Thread *new_owner;
    };
    auto lock_for_park_and_assign_owner_or_backoff(Thread *const, Thread *new_owner)
        -> ustl::Option<ParkAndAssignOwner>;

    template <typename UnparkThreads> // Thread::UnparkList
    auto lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
        -> ustl::Option<UnparkThreads>;

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
