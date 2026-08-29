#include "ours/task/sched/schedule.hpp"
#include "ours/task/sched/scheduler.hpp"

#include <ours/task/sched/scheduler_inline.hpp>
#include <ours/task/owned_wait_queue.hpp>

namespace ours::task::sched {
/// CRTP base class.
class Scheduler::Pi {
    using Self = Scheduler::Pi;
  public:
    template <typename Upstream, typename UpdateDynamicParams>
    static auto common(Upstream &upstream, Thread &thread, 
                       UpdateDynamicParams update_dynamic_params) -> void;

    template <typename Upstream, typename UpdateDynamicParams>
    static auto common(Upstream &upstream, OwnedWaitQueue &owq, 
                       UpdateDynamicParams update_dynamic_params) -> void;
};

template <typename Upstream, typename UpdateDynamicParams>
auto Scheduler::Pi::common(Upstream &upstream, Thread &thread, 
                           UpdateDynamicParams update_dynamic_params) -> void {
    auto &context = thread.scheduling_context();
    auto &profile = context.m_profile;

    // Is the thread in active state?
    if (auto const which_cpu = context.curr_cpu(); which_cpu != INVALID_CPU_NUM) {
        auto &scheduler = Scheduler::Current::get(which_cpu);
        Guard guard{scheduler.get_lock()};

        scheduler.assert_thread_in_scheduler(&thread);

        auto const state = thread.state();
        if (state == Thread::State::Ready) {
            using enum DequeueControl;
            // We are in queue. The change of it's priority must effects on where to place this thread.
            scheduler.dequeue_thread(&thread, Save | SkipClock);
        } else if (state == Thread::State::Running) {
            scheduler.put_prev_thread(&thread);
        }

        auto const oldep = profile.effective;
        profile.recompute_effective_profile();
        auto const newep = profile.effective;

        update_dynamic_params(oldep, newep);

        if (state == Thread::State::Ready) {
            using enum EnqueueControl;
            scheduler.enqueue_thread(&thread, Restore | SkipClock);
        } else if (state == Thread::State::Running) {
            scheduler.set_next_thread(&thread);
        }

        // Almost finished. Try once to preempt.
    } else {
        auto const oldep = profile.effective;
        if (auto wq = thread.waiter_state().wait_queue(); wq != nullptr) {
            // If the thread is on a wait queue, then the change on it's profile will affects
            // wait queue's profile.
            wq->get_lock().assert_held();
            wq->update_parked_thread_effective_profile(&thread);
        } else {
            profile.recompute_effective_profile();
        }
        update_dynamic_params(oldep, profile.effective);
    }
}

template <typename Upstream, typename UpdateDynamicParams>
auto Scheduler::Pi::common(Upstream &upstream, OwnedWaitQueue &owq,
                           UpdateDynamicParams update_dynamic_params) -> void {

}

auto Scheduler::on_thread_base_profile_overrided(Thread &thread) -> void {
}

template <typename Target>
auto Scheduler::on_upstream_thread_base_profile_overrided(Thread &thread, Target &target) -> void {
}

template <typename Upstream, typename Target>
auto Scheduler::join_node_to_pi_graph(Upstream &upstream, Target &target) -> void {
}

template <typename Upstream, typename Target>
auto Scheduler::split_node_from_pi_graph(Upstream &upstream, Target &target) -> void {
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