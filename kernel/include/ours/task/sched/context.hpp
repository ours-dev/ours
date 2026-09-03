/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_CONTEXT_HPP
#define OURS_TASK_SCHED_CONTEXT_HPP 1

#include <gktl/mixin/comparable.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/sync/types.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/sched/profiles.hpp>
#include <ours/task/sched/types.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>
#include <ustl/sync/atomic.hpp>

// #include <cnl/fraction.hpp>

namespace ours::task::sched {
class CpuAffinity {
    using Self = CpuAffinity;

  public:
    enum class Kind { Hard, Soft };

    FORCE_INLINE auto set_affinity_soft(CpuMask const &affinity_mask) -> void {
        m_soft_affinity = &affinity_mask;
    }

    FORCE_INLINE auto set_affinity_hard(CpuMask const &affinity_mask) -> void {
        m_hard_affinity = &affinity_mask;
    }

    auto get_available_mask(CpuMask const &mask) -> CpuMask {
        auto const available_mask = mask & *m_hard_affinity;
        if (auto const available_mask_relaxed = available_mask & *m_soft_affinity;
            available_mask_relaxed.any()) {
            return available_mask_relaxed;
        }

        return available_mask;
    }

  private:
    friend class Scheduler;

    CpuMask const *m_soft_affinity;
    CpuMask const *m_hard_affinity;
};

class RunQueueState {
    using Self = RunQueueState;

  public:
    /// TODO(SmallHuaZi): two gaps in this state machine, see the header comment
    /// of scheduler.cpp for the surrounding semantics.
    ///   1. DequeueDelayed has neither a setter nor a predicate below, so the
    ///      Status::Delayed path of Scheduler::reschedule_common cannot record
    ///      itself and leaves the object stuck in Rescheduling.
    ///   2. Rescheduling has no clearing point: a thread that is merely
    ///      preempted and re-queued (Scheduler::put_prev_thread) keeps it
    ///      forever, since only activate_thread sets Queueing back.
    enum class TransientState : u8 {
        /// The object is not on runqueue.
        None,
        /// The object is on runqueue.
        Queueing,
        /// The object was left on the runqueue by a deliberately delayed
        /// dequeue (see Fair::try_to_delay_dequeue, SCHED_DELAY_DEQUEUE).
        DequeueDelayed,
        /// The object is been migrating onto another processor. Means
        /// "runnable, in flight between runqueues" -- never merely "off the
        /// runqueue", which is None.
        Migrating,
        /// The object is been rescheduling by current scheduler.
        Rescheduling,
    };

    /// Is it able to enqueue itself?
    auto is_active() const -> bool {
        return m_queue_state == Disposition::Enqueued;
    }

    auto on_enqueue() -> void {
        m_queue_state = Disposition::Enqueued;
    }

    auto on_dequeue() -> void {
        m_queue_state = Disposition::Associated;
    }

    auto is_transient_state_queueing() const -> bool {
        return m_transient_state == TransientState::Queueing;
    }

    auto is_transient_state_migrating() const -> bool {
        return m_transient_state == TransientState::Migrating;
    }

    auto is_transient_state_rescheduling() const -> bool {
        return m_transient_state == TransientState::Rescheduling;
    }

    /// Called only by scheduler
    auto set_transient_state_none() -> void {
        m_transient_state = TransientState::None;
    }

    auto set_transient_state_queueing() -> void {
        m_transient_state = TransientState::Queueing;
    }

    auto set_transient_state_migrating() -> void {
        m_transient_state = TransientState::Migrating;
    }

    auto set_transient_state_rescheduling() -> void {
        m_transient_state = TransientState::Rescheduling;
    }

  private:
    friend class Context;
    friend class Scheduler;

    enum class Disposition : u8 {
        Unassociated,
        Associated,
        Enqueued,
        Stolen,
    };
    Disposition m_queue_state{Disposition::Unassociated};
    TransientState m_transient_state{TransientState::None};
};

struct SchedParams {
    SchedTime deadline;
    SchedDuration prev_runtime;
    SchedDuration runtime;
    SchedDuration vruntime;
    SchedDuration vdeadline;
    SchedDuration vlag;
    usize generations;
};

struct Metric {
    auto inc_context_switch_count(bool is_involuntary) -> void {
        if (is_involuntary) {
            num_uneager_context_switch += 1;
        } else {
            num_eager_context_switch += 1;
        }
    }

    usize num_eager_context_switch;
    usize num_uneager_context_switch;
};

class Timeslice {
  public:
    constexpr static auto TIMESLICE_EXTENSION_BIT = BIT(31);

    constexpr static auto TIMESLICE_CUSTOM_BIT = BIT(30);

    auto evaluate() -> bool;

    auto length() const -> SchedDuration {
        return SchedDuration(m_base);
    }

    auto has_timelice_extension() const -> bool {
        return m_extension & TIMESLICE_EXTENSION_BIT;
    }

    auto is_custom_timeslice() const -> bool {
        return m_extension & TIMESLICE_CUSTOM_BIT;
    }

  private:
    friend class Scheduler;
    friend class Fair;

    u32 m_base;
    u32 m_extension;
};

class SchedulingState {
    using Self = SchedulingState;

  public:
  private:
}; // class SchedulingState

class Context: public gktl::mixin::Comparable<Context> {
    using Self = Context;

  public:
    Context(BaseProfile profile)
        : m_affinity(), m_runqueue_state(), m_runqueue_hook(), m_metric(), m_profile(profile) {}

    FORCE_INLINE auto discipline() const -> Discipline {
        return m_profile.effective.discipline;
    }

    FORCE_INLINE auto deadline() const -> SchedTime {
        return m_params.deadline;
    }

    FORCE_INLINE auto runtime() const -> SchedDuration {
        return m_params.runtime;
    }

    FORCE_INLINE auto weight() -> Weight {
        return m_profile.effective.weight;
    }

    FORCE_INLINE auto metric() -> Metric & {
        return m_metric;
    }

    FORCE_INLINE auto last_cpu() const -> CpuNum {
        return m_last_cpu;
    }

    FORCE_INLINE auto curr_cpu() const -> CpuNum {
        return m_curr_cpu;
    }

    FORCE_INLINE auto schedule() -> ISchedule * {
        return m_schedule;
    }

    FORCE_INLINE auto runqueue_state() -> RunQueueState & {
        return m_runqueue_state;
    }

    FORCE_INLINE auto cpu_affinity() -> CpuAffinity & {
        return m_affinity;
    }

    template <typename NodeTraits>
    struct WaitQueueTraits;

  private:
    friend class Scheduler;
    friend class Fair;
    friend class task::OwnedWaitQueue;

    CpuAffinity m_affinity;
    RunQueueState m_runqueue_state;
    ustl::collections::intrusive::SetMemberHook<> m_runqueue_hook;

    Metric m_metric;

    Profile m_profile;
    CpuNum m_last_cpu;
    CpuNum m_curr_cpu;
    CpuNum m_wake_cpu;
    ISchedule *m_schedule;
    SchedParams m_params;
    Timeslice m_timeslice;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_runqueue_hook, RunQueueOption);

    template <typename = void>
    using RunQueueTraits =
        ustl::collections::intrusive::Mhtraits<Self, decltype(Self::m_runqueue_hook),
                                               &Self::m_runqueue_hook>;
};

template <typename NodeTraits>
struct Context::WaitQueueTraits {};

} // namespace ours::task::sched

#endif // OURS_TASK_SCHED_CONTEXT_HPP
