#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

#include <type_traits>
#include <utility>

#include <gktl/counter.hpp>
#include <ustl/util/tuple.hpp>

using ours::task::sched::BaseProfile;
using ours::task::sched::InheritedProfile;

using ustl::traits::IsSameV;

namespace ours::task {
namespace {
    GKTL_COUNTER(PI_PROMOTIONS, "kernel.pi.adjustment.promotions");
    GKTL_COUNTER(PI_DEMOTIONS, "kernel.pi.adjustment.demotions");
    GKTL_COUNTER(PI_BASE_PROFILE_OVERRIDED, "kernel.pi.adjustment.base_profile_overrided");

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    [[maybe_unused]]
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::REMOVE_SINGLE_EDGE_OP)) -> void {
        Scheduler::split_node_from_pi_graph(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            PI_DEMOTIONS.add(1);
        }
    }

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    [[maybe_unused]]
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::ADD_SINGLE_EDGE_OP)) -> void {
        Scheduler::join_node_to_pi_graph(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            PI_PROMOTIONS.add(1);
        }
    }

    template <OwnedWaitQueue::PropagateOp Op, typename Upstream, typename Downstream>
        requires PropagatingStream<Upstream, Downstream>
    [[maybe_unused]]
    auto propagate(Upstream &upstream, Downstream &downstream,
                   decltype(OwnedWaitQueue::BASE_PROFILE_OVERRIDED_OP)) -> void {
        Scheduler::on_upstream_thread_base_profile_overrided(upstream, downstream);
        if constexpr (IsSameV<Downstream, Thread>) {
            PI_BASE_PROFILE_OVERRIDED.add(1);
        }
    }

} // namespace

OwnedWaitQueue::OwnedWaitQueue() : Base(Type::Owned), m_owner(), m_managed_hook() {}

OwnedWaitQueue::~OwnedWaitQueue() {
    DEBUG_ASSERT(m_owner == nullptr);
}

/// The hook a plain wake uses: every waiter is eligible, nothing extra
/// happens, and nobody is requeued.
///
/// One shared instance at namespace scope rather than a function-local static
/// -- a local one would need a guard variable and run its constructor on first
/// use, which is not something to do underneath a lock.
namespace {
    OwnedWaitQueue::INotifyRequeueHook DEFAULT_WAKE_HOOK{
        .allow = [] { return true; },
        .on_notify = [] { return true; },
        .on_requeue = [] { return false; },
    };
} // namespace

auto OwnedWaitQueue::default_wake_hook() -> INotifyRequeueHook & {
    return DEFAULT_WAKE_HOOK;
}

/// Copies out a thread's inherited profile so a propagation can compare the
/// before and after states; see begin_propagate/complete_propagate.
auto OwnedWaitQueue::snapshot_thread_inherited_profile(Thread *thread) -> sched::InheritedProfile {
    return thread->scheduling_entity().m_profile.inherited.snapshot();
}

/// Collects up to |max_notify| waiters to wake, leaving the queue lock held
/// for the notify_threads_locked() that follows.
///
/// TODO(SmallHuaZi): a placeholder, and deliberately shaped like the base
/// class's own: WaitQueue::lock_for_notify_one/all are themselves stubs that
/// return none(), so the whole notify path is unwritten -- there is nothing
/// underneath this to build on yet. Until it exists, Mutex::release_slowpath()
/// finds no one to hand the mutex to and simply drops it to Available.
///
/// Writing it for real needs two decisions first: where the queue lock is
/// acquired and released across this pair of calls, and how INotifyRequeueHook
/// interacts with the priority-inheritance propagation in begin_propagate().
template <typename UnparkThreads>
auto OwnedWaitQueue::lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
    -> ustl::Option<UnparkThreads> {
    return ustl::NONE;
}

template <typename UnparkThreads>
auto OwnedWaitQueue::notify_threads_locked(UnparkThreads, INotifyRequeueHook &, NotifyControl)
    -> void {}

template auto OwnedWaitQueue::lock_for_notify_operation<Thread::UnparkList>(usize,
                                                                            INotifyRequeueHook &)
    -> ustl::Option<Thread::UnparkList>;
template auto OwnedWaitQueue::notify_threads_locked(Thread::UnparkList, INotifyRequeueHook &,
                                                    NotifyControl) -> void;

/// Both halves of an ownership edge run under the owner thread's PI lock:
/// the list they splice into is that thread's outgoing edge set in the
/// profile inheritance graph, and a PI chain walk reaches a thread holding
/// nothing but its PI lock. See the note on Thread::m_owned_wait_queues for
/// why the resource lock was the wrong place for it.
///
/// The precondition cannot be spelled as TA_REQUIRES. The lock is reached
/// through a pointer whose value the analysis cannot follow -- and for
/// reset_owner() the pointer is a member, so there is no expression at the
/// call site to name -- which is why the check is a run-time assert_held()
/// and the function opts out of the static analysis instead.
TA_NO_THREAD_SAFETY_ANALYSIS
auto OwnedWaitQueue::set_owner(Thread *new_owner) -> void {
    DEBUG_ASSERT(m_owner == nullptr);
    new_owner->get_pi_lock().assert_held();

    new_owner->m_owned_wait_queues.push_back(*this);
    m_owner = new_owner;
}

TA_NO_THREAD_SAFETY_ANALYSIS
auto OwnedWaitQueue::reset_owner() -> void {
    DEBUG_ASSERT(m_owner != nullptr);
    m_owner->get_pi_lock().assert_held();

    static_cast<void>(
        m_owner->m_owned_wait_queues.erase(m_owner->m_owned_wait_queues.iterator_to(*this)));
    m_owner = nullptr;
}

/// Drops every ownership edge leaving |thread|. A thread on its way out has to
/// do this before its storage can go back: an OwnedWaitQueue still pointing at
/// it would hand that dead pointer to the next propagation that walks through
/// the queue.
///
/// The PI lock is held across the whole drain rather than swapping the list
/// out first. The swap was not just a lock-scope shortcut, it was wrong:
/// reset_owner() unlinks the queue from `m_owner->m_owned_wait_queues`, but
/// after a swap the queue is no longer on that list -- it is on the local one,
/// and pop_front() had already unlinked it from there. The erase then ran
/// against a list the node was not a member of. Holding the lock instead lets
/// reset_owner() stay the single place that unlinks a queue, and the loop just
/// re-reads the head until the list runs dry.
///
/// Lock order is the thread's PI lock first and the queue's lock underneath,
/// the same direction WaitQueue::enqueue_waiter takes them; see the note
/// there.
TA_NO_THREAD_SAFETY_ANALYSIS
auto OwnedWaitQueue::disown_all_queues(Thread *thread) -> void {
    Guard guard{thread->get_pi_lock()};

    while (!thread->m_owned_wait_queues.empty()) {
        auto &owq = *thread->m_owned_wait_queues.front();
        Guard queue_guard{owq.get_lock()};
        owq.reset_owner();
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
        owq->verify_owned();
        oldip = Self::snapshot_thread_inherited_profile(thread);
    }

    auto &context = thread->scheduling_entity();
    context.m_profile.base = profile;
    context.m_profile.effective.notify_base_profile_overrided();
    Scheduler::on_thread_base_profile_overrided(*thread);

    if (owq) {
        auto newip = Self::snapshot_thread_inherited_profile(thread);
        Self::complete_propagate(*thread, *owq, &newip, &oldip, BASE_PROFILE_OVERRIDED_OP);
    }

    PI_BASE_PROFILE_OVERRIDED.add(1);
}

auto OwnedWaitQueue::assign_owner(Thread *new_owner) -> Status {
    verify_owned();

    if (m_owner == new_owner) {
        return Status::Ok;
    }

    Self::lock_for_owner_replacement(new_owner, 0);
    if (m_owner != nullptr) {
        m_owner->get_pi_lock().assert_held();
        Self::begin_propagate(*this, *m_owner, REMOVE_SINGLE_EDGE_OP);
        Self::unlock_pi_chain_common(this, 0);
    }

    if (new_owner != nullptr) {
        new_owner->get_pi_lock().assert_held();
        Self::begin_propagate(*this, *new_owner, ADD_SINGLE_EDGE_OP);
        Self::unlock_pi_chain_common(this, 0);
    }

    return Status::Ok;
}

auto OwnedWaitQueue::lock_for_owner_replacement(Thread *const new_owner, Thread *) -> void {}

auto OwnedWaitQueue::lock_for_park_and_assign_owner_or_backoff(Thread *const curr,
                                                               Thread *const new_owner)
    -> ustl::Option<ParkAndAssignOwner> {
    return ParkAndAssignOwner{new_owner};
}

/// Blocks `curr` on this queue until a waker hands it whatever the queue
/// guards.
///
/// The name promises two things and currently delivers one. The parking is
/// real: it goes through WaitQueue::park_thread(), so the thread leaves the
/// run queue and the caller's activation does not come back until somebody
/// notifies it. The owner assignment is not armed, and that is deliberate
/// rather than forgotten -- see the note below.
///
/// The PI lock is taken here rather than asked of the caller, and it is taken
/// with interrupts and preemption disabled, because the guard is carried into
/// reschedule_common() through park_thread() and that is the state it
/// requires. Event::wait_etc() takes it the same way for the same reason.
auto OwnedWaitQueue::park_and_assign_owner(Thread *const curr, time::Deadline const &deadline,
                                           Interruptible interruptible) -> Status {
    verify_owned();

    // Not assign_owner(new_owner) yet. That path walks the PI chain --
    // begin_propagate() asserts the owner's PI lock is already held -- and
    // acquiring a chain of thread locks without a global thread_lock needs the
    // ChainLock-style backoff that lock_for_park_and_assign_owner_or_backoff()
    // is a placeholder for. Blocking without inheritance is a correct mutex
    // that can invert priorities; blocking with a half-built chain walk is a
    // deadlock. Recorded in docs/repairs.
    Guard<sync::SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{curr->get_pi_lock()};
    return park_thread(curr, deadline, Signals::None, interruptible);
}

template <typename Upstream, typename Downstream>
constexpr static auto FromThreadToOwq =
    std::is_same_v<Upstream, Thread> && std::is_same_v<Downstream, OwnedWaitQueue>;

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
        inherited_profile = thread.scheduling_entity().m_profile.inherited.snapshot();
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
auto OwnedWaitQueue::lock_pi_chain_common(Applicant *start) -> void {
    auto [thread, owq] = ustl::make_tuple<Thread *, Self *>(0, 0);
    if constexpr (IsSameV<Applicant, Thread>) {
        thread = start;
    } else {
        owq = start;
        goto start_from_owq;
    }

    while (true) {
        if (!thread->get_pi_lock().try_acquire()) {
        }

        thread = owq->m_owner;

    [[maybe_unused]] start_from_owq:
    }
}

template <typename Applicant>
auto OwnedWaitQueue::unlock_pi_chain_common(Applicant *new_owner, void *stop_point) -> void {}

/// Exported to WaitQueue::unpark_thread
template auto OwnedWaitQueue::begin_propagate(Thread &, OwnedWaitQueue &,
                                              decltype(REMOVE_SINGLE_EDGE_OP)) -> void;

} // namespace ours::task
