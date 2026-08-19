#include <gktl/counter.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/owned-wait-queue.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ustl/util/tuple.hpp>

using ours::task::Scheduler;
using ours::task::Thread;
using ours::task::sched::BaseProfile;
using ours::task::sched::InheritedProfile;

using ustl::traits::IsSameV;

namespace ours::sync {
namespace {
    GKTL_COUNTER(pi_promotions, "kernel.pi.adjustment.promotions");
    GKTL_COUNTER(pi_demotions, "kernel.pi.adjustment.demotions");
    GKTL_COUNTER(pi_base_profile_overrided, "kernel.pi.adjustment.base_profile_overrided");

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::kRemoveSingleEdgeOp)) -> void {
        Scheduler::split_node_from_pi_graph(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            pi_demotions.add(1);
        }
    }

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::kAddSingleEdgeOp)) -> void {
        Scheduler::join_node_to_pi_graph(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            pi_promotions.add(1);
        }
    }

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::kBaseProfileOverridedOp)) -> void {
        Scheduler::on_upstream_thread_base_profile_overrided(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            pi_base_profile_overrided.add(1);
        }
    }

} // namespace

OwnedWaitQueue::OwnedWaitQueue() : Base(kOwnedMagic), m_owner(), m_managed_hook() {}

OwnedWaitQueue::~OwnedWaitQueue() {
    DEBUG_ASSERT(m_owner == nullptr);
}

auto OwnedWaitQueue::set_owner(Thread *new_owner) -> void {
    DEBUG_ASSERT(m_owner == nullptr);
    new_owner->m_owned_wait_queues.push_back(*this);
    m_owner = new_owner;
}

auto OwnedWaitQueue::reset_owner() -> void {
    DEBUG_ASSERT(m_owner->get_resource_lock().is_locked());
    static_cast<void>(
        m_owner->m_owned_wait_queues.erase(m_owner->m_owned_wait_queues.iterator_to(*this)));
    m_owner = nullptr;
}

auto OwnedWaitQueue::disown_all_queues(Thread *thread) -> void {
    Self::List owqs;
    {
        Guard guard{thread->get_resource_lock()};
        owqs.swap(thread->m_owned_wait_queues);
    }

    while (!owqs.empty()) {
        auto &owq = *owqs.front();
        owqs.pop_front();
        {
            Guard guard{owq.get_lock()};
            owq.reset_owner();
        }
    }
}

auto OwnedWaitQueue::override_thread_base_profile(Thread *thread, BaseProfile const &profile)
    -> void {
    // Acquires |thread|'s lock before to get the wait queue it owned.
    Guard guard{thread->get_pi_lock()};

    auto const wq = thread->waiter_state().wait_queue();
    auto const owq = as<OwnedWaitQueue>(wq);

    InheritedProfile oldip;
    if (owq) {
        owq->m_canary.verify(kOwnedMagic);
        oldip = Self::snapshot_thread_inherited_profile(thread);
    }

    auto &context = thread->scheduling_context();
    context.m_profile.base = profile;
    context.m_profile.effective.notify_base_profile_overrided();
    Scheduler::on_thread_base_profile_overrided(*thread);

    if (owq) {
        auto newip = Self::snapshot_thread_inherited_profile(thread);
        Self::complete_propagate(*thread, *owq, &newip, &oldip, kBaseProfileOverridedOp);
    }

    pi_base_profile_overrided.add(1);
}

auto OwnedWaitQueue::assign_owner(Thread *new_owner) -> Status {
    m_canary.verify<Self>();

    if (m_owner == new_owner) {
        return Status::Ok;
    }

    Self::lock_for_owner_replacement(new_owner, 0);
    if (m_owner != nullptr) {
        m_owner->get_pi_lock().assert_held();
        Self::begin_propagate(*this, *m_owner, kRemoveSingleEdgeOp);
        Self::unlock_pi_chain_common(this, 0);
    }

    if (new_owner != nullptr) {
        new_owner->get_pi_lock().assert_held();
        Self::begin_propagate(*this, *new_owner, kAddSingleEdgeOp);
        Self::unlock_pi_chain_common(this, 0);
    }

    return Status::Ok;
}

auto OwnedWaitQueue::lock_for_owner_replacement(Thread *new_owner, Thread *) -> void {}

auto OwnedWaitQueue::park_and_assign_owner(Thread *const curr, time::Deadline const &,
                                           Interruptible interruptible) -> Status {
    m_canary.verify(kOwnedMagic);
    return Status::Ok;
}

template <typename Upstream, typename Downstream>
constexpr static auto const FromThreadToOwq =
    IsSameV<Upstream, Thread> && IsSameV<Downstream, OwnedWaitQueue>;

template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
    requires PropagatingStream<Upstream, Downstream>
auto OwnedWaitQueue::begin_propagate(Upstream &upstream, Downstream &downstream,
                                     PropagateTag<Op> op) -> void {
    static_assert(Op != PropagateOp::BaseProfileOverrided);

    InheritedProfile inherited_profile;
    if constexpr (FromThreadToOwq<Upstream, Downstream>) { // From Thread to OWQ.
        Self &owq = downstream;                            // Type annotation.
        Thread &thread = upstream;                         // Type annotation.

        if (Op == PropagateOp::RemoveSingleEdge) {
            if (owq.is_empty() && owq.m_owner == nullptr) {
                return;
            }
        }
        inherited_profile = thread.scheduling_context().m_profile.inherited.snapshot();
    } else {                         // From OWQ to Thread.
        Self &owq = upstream;        // Type annotation.
        Thread &thread = downstream; // Type annotation.

        if constexpr (Op == PropagateOp::AddSingleEdge) {
            owq.set_owner(&thread);
        } else if constexpr (Op == PropagateOp::RemoveSingleEdge) {
            owq.reset_owner();
        }

        if (owq.is_empty()) {
            return;
        }

        inherited_profile = owq.m_ipv;
    }

    if constexpr (Op == PropagateOp::AddSingleEdge) {
        Self::complete_propagate(upstream, downstream, &inherited_profile, 0, op);
    } else if constexpr (Op == PropagateOp::RemoveSingleEdge) {
        Self::complete_propagate(upstream, downstream, 0, &inherited_profile, op);
    }
}

template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
    requires PropagatingStream<Upstream, Downstream>
auto OwnedWaitQueue::complete_propagate(Upstream &upstream, Downstream &downstream,
                                        InheritedProfile const *added, InheritedProfile const *lost,
                                        PropagateTag<Op> op) -> void {
    auto [thread, owq] = ustl::make_tuple<Thread *, Self *>(0, 0);
    if constexpr (FromThreadToOwq<Upstream, Downstream>) {
        owq = &downstream;
        thread = &upstream;
    } else {
        owq = &upstream;
        thread = &downstream;
    }
}

template <typename Applicant>
auto OwnedWaitQueue::lock_pi_chain_common(Applicant *start, Thread *blocking_thread) -> void {
    auto [thread, owq] = ustl::make_tuple<Thread *, Self *>(0, 0);
    if constexpr (IsSameV<Applicant, Thread>) {
        thread = start;
    } else {
        owq = start;
        goto start_from_owq;
    }

    while (true) {
        if (thread == nullptr) {
            return;
        }

        if (!thread->get_pi_lock().try_acquire()) {
        }

        thread = owq->m_owner;
    [[maybe_unused]] start_from_owq:
        if (owq == nullptr) {
            return;
        }
    }
}

template <typename Applicant>
auto OwnedWaitQueue::unlock_pi_chain_common(Applicant *new_owner, void *stop_point) -> void {}

/// Exported to WaitQueue::unpark_thread
template auto OwnedWaitQueue::begin_propagate(Thread &, OwnedWaitQueue &,
                                              decltype(kRemoveSingleEdgeOp)) -> void;

} // namespace ours::sync
