#include <ours/task/sched/scheduler-inline.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ustl/traits/is_same.hpp>

using ours::sync::OwnedWaitQueue;
using ustl::traits::IsSameV;

namespace ours::task::sched {
/// CRTP base class.
template <typename Op, typename Target>
class Scheduler::PiOperation {
    using Self = PiOperation;

  public:
    PiOperation(Target &target) : m_target(target), m_now() {}

    ~PiOperation() = default;

    auto handle_pi_interaction_common() -> void;

    auto update_dynamic_params(EffectiveProfile const &oldep, EffectiveProfile const &newep)
        -> void {
        return static_cast<Op *>(this)->update_dynamic_params(oldep, newep);
    }

  protected:
    Target &m_target;
    SchedTime m_now;
};

template <typename Op, typename Target>
auto Scheduler::PiOperation<Op, Target>::handle_pi_interaction_common() -> void {
    if constexpr (IsSameV<Target, Thread>) {
        Thread *thread = &m_target; // Type annotation.

        auto &context = thread->scheduling_context();
        auto &profile = context.m_profile;

        // Is the thread in active state?
        if (auto const which_cpu = context.curr_cpu(); which_cpu != kInvalidCpuNum) {
            auto &scheduler = Scheduler::Current::get(which_cpu);
            Guard guard{scheduler.get_lock()};

            scheduler.assert_thread_in_scheduler(thread);

            auto const state = thread->state();
            if (state == Thread::State::Ready) {
                scheduler.dequeue_thread(thread, DequeueControl::Save | DequeueControl::SkipClock);
            }
            if (state == Thread::State::Running) {
                scheduler.put_prev_thread(thread);
            }

            auto const oldep = profile.effective;
            profile.recompute_effective_profile();
            auto const newep = profile.effective;

            update_dynamic_params(oldep, newep);

            if (state == Thread::State::Ready) {
                scheduler.enqueue_thread(thread,
                                         EnqueueControl::Restore | EnqueueControl::SkipClock);
            }
            if (state == Thread::State::Running) {
                scheduler.set_next_thread(thread);
            }
        } else {
            auto const oldep = profile.effective;
            if (auto wq = thread->waiter_state().wait_queue(); wq != nullptr) {
                // If the thread is on a wait queue, then the change on it's profile will affects
                // wait queue's profile.
                wq->get_lock().assert_held();
                wq->update_parked_thread_effective_profile(thread);
            } else {
                profile.recompute_effective_profile();
            }
            update_dynamic_params(oldep, profile.effective);
        }
    } else {
        // update_dynamic_params(oldep, profile.effective);
    }
}

class OnThreadBaseProfileOverridedOp
    : public Scheduler::PiOperation<OnThreadBaseProfileOverridedOp, Thread> {
    using Base = Scheduler::PiOperation<OnThreadBaseProfileOverridedOp, Thread>;

  public:
    using Base::Base;

    auto update_dynamic_params(EffectiveProfile const &oldep, EffectiveProfile const &newep)
        -> void;

    auto do_operation() -> void {
        Base::handle_pi_interaction_common();
    }
};

template <typename Target>
class OnUpstreamThreadBaseProfileOverridedOp
    : public Scheduler::PiOperation<OnUpstreamThreadBaseProfileOverridedOp<Target>, Target> {
    using Base = Scheduler::PiOperation<OnUpstreamThreadBaseProfileOverridedOp, Target>;

  public:
    OnUpstreamThreadBaseProfileOverridedOp(Thread &thread, Target &target)
        : Base(target), m_thread(thread) {}

    ~OnUpstreamThreadBaseProfileOverridedOp() = default;

    auto update_dynamic_params(EffectiveProfile const &oldep, EffectiveProfile const &newep)
        -> void;

    auto do_operation() -> void {
        Base::handle_pi_interaction_common();
    }

  private:
    Thread &m_thread;
};

template <typename Upstream, typename Target>
class JoinNodeToPiGraphOp
    : public Scheduler::PiOperation<JoinNodeToPiGraphOp<Upstream, Target>, Target> {
    using Base = Scheduler::PiOperation<JoinNodeToPiGraphOp, Target>;

  public:
    JoinNodeToPiGraphOp(Upstream &upstream, Target &target) : Base(target), m_upstream(upstream) {}

    ~JoinNodeToPiGraphOp() = default;

    auto update_dynamic_params(EffectiveProfile const &oldep, EffectiveProfile const &newep)
        -> void;

    auto do_operation() -> void {
        Base::handle_pi_interaction_common();
    }

  private:
    Upstream &m_upstream;
};

template <typename Upstream, typename Target>
class SplitNodeFromPiGraphOp
    : public Scheduler::PiOperation<SplitNodeFromPiGraphOp<Upstream, Target>, Target> {
    using Base = Scheduler::PiOperation<SplitNodeFromPiGraphOp, Target>;

  public:
    SplitNodeFromPiGraphOp(Upstream &upstream, Target &target)
        : Base(target), m_upstream(upstream) {}

    ~SplitNodeFromPiGraphOp() = default;

    auto update_dynamic_params(EffectiveProfile const &oldep, EffectiveProfile const &newep)
        -> void;

    auto do_operation() -> void {
        Base::handle_pi_interaction_common();
    }

  private:
    Upstream &m_upstream;
};

auto Scheduler::on_thread_base_profile_overrided(Thread &thread) -> void {
    OnThreadBaseProfileOverridedOp op{thread};
    op.do_operation();
}

template <typename Target>
auto Scheduler::on_upstream_thread_base_profile_overrided(Thread &thread, Target &target) -> void {
    OnUpstreamThreadBaseProfileOverridedOp op{thread, target};
    op.do_operation();
}

template <typename Upstream, typename Target>
auto Scheduler::join_node_to_pi_graph(Upstream &upstream, Target &target) -> void {
    JoinNodeToPiGraphOp op{upstream, target};
    op.do_operation();
}

template <typename Upstream, typename Target>
auto Scheduler::split_node_from_pi_graph(Upstream &upstream, Target &target) -> void {
    SplitNodeFromPiGraphOp op{upstream, target};
    op.do_operation();
}

template auto Scheduler::on_upstream_thread_base_profile_overrided(Thread &, Thread &) -> void;
template auto Scheduler::on_upstream_thread_base_profile_overrided(Thread &, OwnedWaitQueue &)
    -> void;

template auto Scheduler::join_node_to_pi_graph(Thread &, Thread &) -> void;
template auto Scheduler::join_node_to_pi_graph(Thread &, OwnedWaitQueue &) -> void;
template auto Scheduler::join_node_to_pi_graph(OwnedWaitQueue &, Thread &) -> void;
template auto Scheduler::join_node_to_pi_graph(OwnedWaitQueue &, OwnedWaitQueue &) -> void;

template auto Scheduler::split_node_from_pi_graph(Thread &, Thread &) -> void;
template auto Scheduler::split_node_from_pi_graph(Thread &, OwnedWaitQueue &) -> void;
template auto Scheduler::split_node_from_pi_graph(OwnedWaitQueue &, Thread &) -> void;
template auto Scheduler::split_node_from_pi_graph(OwnedWaitQueue &, OwnedWaitQueue &) -> void;
} // namespace ours::task::sched