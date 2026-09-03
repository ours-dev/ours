/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_CONTEXT_HPP
#define OURS_TASK_SCHED_CONTEXT_HPP 1

#include <gktl/mixin/comparable.hpp>
#include <ours/cpu.hpp>
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

    /// Each affinity is a mask this object owns plus a pointer that usually
    /// aims at it, which is the shape Linux gives task_struct with its
    /// `cpumask_t *cpus_ptr` beside an owned `cpumask_t cpus_mask`.
    ///
    /// The pointer alone was not enough. It is non-owning, and nothing in
    /// `set_affinity_hard(CpuMask const &)` told a caller that the argument
    /// had to outlive the thread, so all three callers passed a temporary
    /// `CpuMask::of(n)` and left the thread holding a dangling pointer the
    /// moment the statement ended. An interface that every one of its callers
    /// used incorrectly is the thing at fault, not the callers.
    ///
    /// Owning the mask is not enough either, because one of the masks stored
    /// here must NOT be copied: a thread whose affinity is "wherever is
    /// online" has to keep tracking Cpu::online_mask() as the secondaries come
    /// up, and a copy would freeze it at whatever was online when the thread
    /// was created -- on the boot path, the boot CPU alone, forever. So both
    /// forms exist and the safe one is the one with the plain name.
    ///
    /// Both masks start out unrestricted rather than null. get_available_mask()
    /// dereferences both pointers unconditionally, and a value-initialized
    /// CpuAffinity left them null: every caller that ran before a Scheduler had
    /// installed a real affinity -- which is every caller on the thread-creation
    /// path -- read a CpuMask out of address 0. cpu_all_mask() is the right
    /// default for the reason Linux uses it too: "not yet narrowed" should be a
    /// wider set than any real machine, so a later set only ever restricts it.
    CpuAffinity()
        : m_owned_soft(cpu_all_mask())
        , m_owned_hard(cpu_all_mask())
        , m_soft_affinity(&m_owned_soft)
        , m_hard_affinity(&m_owned_hard) {}

    /// The pointers aim into this object, so neither copying nor moving one
    /// would leave the copy pointing at its own masks. Nothing needs to: a
    /// CpuAffinity lives inside a Thread's scheduling context and is built in
    /// place with it. Deleting these turns a silent aliasing bug into a
    /// compile error if that ever changes.
    CpuAffinity(Self const &) = delete;
    CpuAffinity(Self &&) = delete;
    auto operator=(Self const &) -> Self & = delete;
    auto operator=(Self &&) -> Self & = delete;

    /// Take a copy of |affinity_mask|. Safe for any mask, a temporary
    /// included, and the right choice unless the caller specifically needs
    /// later changes to |affinity_mask| to be followed.
    FORCE_INLINE auto set_affinity_soft(CpuMask const &affinity_mask) -> void {
        m_owned_soft = affinity_mask;
        m_soft_affinity = &m_owned_soft;
    }

    FORCE_INLINE auto set_affinity_hard(CpuMask const &affinity_mask) -> void {
        m_owned_hard = affinity_mask;
        m_hard_affinity = &m_owned_hard;
    }

    /// Track |affinity_mask| in place instead of copying it, so that later
    /// changes to it are changes to this thread's affinity.
    ///
    /// |affinity_mask| MUST outlive the thread -- these are for the masks that
    /// live as long as the kernel does, Cpu::online_mask() above all, whose
    /// whole point here is that it is still growing while early threads exist.
    /// Anything else, including anything built at the call site, wants the
    /// copying form above.
    FORCE_INLINE auto set_affinity_soft_shared(CpuMask const &affinity_mask) -> void {
        m_soft_affinity = &affinity_mask;
    }

    FORCE_INLINE auto set_affinity_hard_shared(CpuMask const &affinity_mask) -> void {
        m_hard_affinity = &affinity_mask;
    }

    [[nodiscard]]
    auto get_available_mask(CpuMask const &mask) const -> CpuMask {
        auto const available_mask = mask & *m_hard_affinity;
        if (auto const available_mask_relaxed = available_mask & *m_soft_affinity;
            available_mask_relaxed.any()) {
            return available_mask_relaxed;
        }

        return available_mask;
    }

  private:
    friend class Scheduler;

    /// Storage for the copying setters. Read through the pointers below, never
    /// directly, because the sharing setters leave these stale on purpose.
    CpuMask m_owned_soft;
    CpuMask m_owned_hard;

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
    /// Every member, in declaration order. The six after m_profile used to be
    /// left out, so a fresh context carried indeterminate values for them --
    /// `m_schedule` among them, and that one is dereferenced. It read as null
    /// in practice only because a Thread comes out of a zeroing allocator;
    /// that is luck, not a guarantee, and it is not something to rely on.
    ///
    /// `m_schedule` stays null until a Scheduler binds the thread to its
    /// discipline in Scheduler::set_schedule(). Nothing may call through it
    /// before that.
    Context(BaseProfile profile)
        : m_affinity()
        , m_runqueue_state()
        , m_runqueue_hook()
        , m_metric()
        , m_profile(profile)
        , m_last_cpu()
        , m_curr_cpu()
        , m_wake_cpu()
        , m_schedule(nullptr)
        , m_params()
        , m_timeslice() {}

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
