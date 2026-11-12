/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHEDULER_STATE_HPP
#define OURS_TASK_SCHEDULER_STATE_HPP 1

#include <ours/chrono.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/sync/types.hpp>
#include <ours/task/scheduler_common.hpp>
#include <ours/task/scheduler_profiles.hpp>
#include <ours/task/scheduler_types.hpp>

#include <atomic>

#include <gktl/mixin/comparable.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>
#include <ustl/collections/intrusive/set_hook.hpp>

// #include <cnl/fraction.hpp>

namespace ours::task {
class CpuAffinity {
    using Self = CpuAffinity;

  public:
    enum class Kind { Hard, Soft };

    /// Each affinity is a mask this object owns plus a pointer that usually
    /// aims at it.
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
    /// default because "not yet narrowed" should be a wider set than any real
    /// machine, so a later set only ever restricts it.
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
    FORCE_INLINE
    auto set_affinity_soft(CpuMask const &affinity_mask) -> void {
        m_owned_soft.store(affinity_mask, std::memory_order_relaxed);
        m_soft_affinity = &m_owned_soft;
    }

    FORCE_INLINE
    auto set_affinity_hard(CpuMask const &affinity_mask) -> void {
        m_owned_hard.store(affinity_mask, std::memory_order_relaxed);
        m_hard_affinity = &m_owned_hard;
    }

    /// Track |affinity_mask| in place instead of copying it, so that later
    /// changes to it are changes to this thread's affinity.
    ///
    /// |affinity_mask| MUST outlive the thread -- these are for the masks that
    /// live as long as the kernel does, Cpu::online_mask_live() above all,
    /// whose whole point here is that it is still growing while early threads
    /// exist. Anything else, including anything built at the call site, wants
    /// the copying form above.
    ///
    /// It is an atomic cell, not a plain mask, because that is what a mask
    /// still being written by other CPUs has to be: the secondaries publish
    /// themselves into Cpu's Online mask from their own bring-up path, so a
    /// thread reading it through a plain reference would be racing them.
    FORCE_INLINE
    auto set_affinity_soft_shared(std::atomic<CpuMask> const &affinity_mask) -> void {
        m_soft_affinity = &affinity_mask;
    }

    FORCE_INLINE
    auto set_affinity_hard_shared(std::atomic<CpuMask> const &affinity_mask) -> void {
        m_hard_affinity = &affinity_mask;
    }

    [[nodiscard]]
    auto get_available_mask(CpuMask const &mask) const -> CpuMask {
        // Relaxed: this only narrows a candidate set, and a CPU that joins
        // between these two loads is one this thread simply was not offered
        // this time round. Nothing is published through the affinity masks, so
        // there is no ordering for an acquire to establish.
        auto const available_mask = mask & m_hard_affinity->load(std::memory_order_relaxed);
        if (auto const available_mask_relaxed =
                available_mask & m_soft_affinity->load(std::memory_order_relaxed);
            available_mask_relaxed.any()) {
            return available_mask_relaxed;
        }

        return available_mask;
    }

  private:
    friend class Scheduler;

    /// Storage for the copying setters. Read through the pointers below, never
    /// directly, because the sharing setters leave these stale on purpose.
    ///
    /// Atomic so that the two pointers can have one type: they aim either here
    /// or at a kernel-lifetime mask that other CPUs write, and the latter has
    /// to be an atomic cell. The owned case never actually contends -- an
    /// affinity is set under the thread's PI lock -- it just pays the same
    /// spelling.
    std::atomic<CpuMask> m_owned_soft;
    std::atomic<CpuMask> m_owned_hard;

    std::atomic<CpuMask> const *m_soft_affinity;
    std::atomic<CpuMask> const *m_hard_affinity;
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
    friend class SchedulerState;
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

/// The Fair discipline's private per-thread state.
///
/// Split out of a single flat `SchedParams` that held these beside the
/// framework's own accounting, seven fields of one type in one struct. That
/// shape is what let `deadline` and `vdeadline` sit next to each other with
/// the compiler unable to say a word about which one a call site meant --
/// `deadline` has no writer anywhere and three readers, `vdeadline` four
/// writers and no reader, and both are `ffl::Fixed<i64, 0>`. Whatever the
/// right answer is there, it is Fair's to give; what the framework can do is
/// stop offering the two of them from the same bag.
///
/// Nothing outside `fair.*` reads these except through two accessors,
/// `virtual_deadline()` (Fair's run-queue comparator and the wait queue's key)
/// and `vruntime()` (Fair's run queue).
///
/// SchedulerState holds one such sub-struct per discipline that has private
/// per-thread state, each owned by the discipline that reads it.
struct FairState {
    SchedTime deadline;
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
    constexpr static auto TIMESLICE_EXTENSION_BIT = 31_bit;

    constexpr static auto TIMESLICE_CUSTOM_BIT = 30_bit;

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
    friend class RoundRobin;

    u32 m_base;
    u32 m_extension;
};

class SchedulingState {
    using Self = SchedulingState;

  public:
  private:
}; // class SchedulingState

class SchedulerState: public gktl::mixin::Comparable<SchedulerState> {
    using Self = SchedulerState;

  public:
    /// Every member, in declaration order. The six after m_profile used to be
    /// left out, so a fresh context carried indeterminate values for them.
    /// That read as zero in practice only because a Thread comes out of a
    /// zeroing allocator; that is luck, not a guarantee, and it is not
    /// something to rely on.
    SchedulerState(BaseProfile profile)
        : m_affinity()
        , m_runqueue_state()
        , m_runqueue_hook()
        , m_rr_hook()
        , m_metric()
        , m_profile(profile)
        // Zero, which is a lie: every reader of these three tests them against
        // INVALID_CPU_NUM to mean "not on a CPU yet", and 0 is a perfectly
        // good CPU number, so "nowhere" and "on the boot CPU" are the same
        // answer here. It is left alone deliberately -- see
        // docs/repairs/260910/task-sched.md. Initialising them honestly turns
        // on three branches that have never executed in this tree (the else
        // arm of Scheduler::Pi::common(), the migration check in
        // set_cpu_affinity(), the "never ran" arm of select_target_cpu()) and
        // the first of them faults, so it is a change that has to be made with
        // those branches, not before them.
        , m_last_cpu()
        , m_curr_cpu()
        , m_wake_cpu()
        , m_exec_runtime()
        , m_prev_exec_runtime()
        , m_fair()
        , m_timeslice() {}

    FORCE_INLINE
    auto discipline() const -> Discipline {
        return m_profile.effective.discipline;
    }

    /// The virtual deadline this thread is ordered by.
    ///
    /// Reaches into FairState because two of its three readers are Fair's own
    /// -- the run-queue comparator and the preemption test -- and both only
    /// have a SchedulerState to work from. The third is WaitQueue, which orders
    /// parked threads by the same key (see the EEVDF note on
    /// WaitQueue::QueueKey).
    ///
    /// It returns `vdeadline`, not `deadline`. The two are the same type and
    /// sat side by side in one flat struct until recently, and the readers had
    /// ended up on the wrong one: `deadline` has no writer anywhere in the
    /// tree, while `vdeadline` is what Fair actually maintains, writing it in
    /// four places. A key that is always zero is not an ordering -- every
    /// comparison tied and the tie-break carried the whole decision, so both
    /// the run queue and the wait queue were in arrival order while their
    /// comments described EEVDF.
    ///
    /// The tell was WaitQueue's own field name: `m_queue_key.vdeadline`, fed
    /// from `deadline()`. The name had said which one it wanted all along.
    ///
    /// No behaviour changes under RoundRobin, which writes neither: the key
    /// stays zero and the queues stay in arrival order, which is the right
    /// answer for a discipline that has no notion of a deadline. It starts
    /// mattering when Fair returns.
    ///
    /// `FairState::deadline` keeps no accessor now. It is left in place per
    /// the scaffolding rule, and it is the field a future Deadline discipline
    /// would want -- at which point it belongs in that discipline's state,
    /// not in Fair's.
    FORCE_INLINE
    auto virtual_deadline() const -> SchedTime {
        return m_fair.vdeadline;
    }

    /// This thread's virtual clock, the one Fair's run queue measures its
    /// average, lag and eligibility on: the real time it has been charged,
    /// scaled by its weight (`to_virtual_time()`), from the queue's average
    /// less the lag it carried when it was last placed (`avg_vruntime() - vlag`,
    /// `Fair::adjust_vlag()`).
    ///
    /// An accessor because `FairRunQueue` is not a friend, for the reason
    /// `virtual_deadline()` is one. It is not comparable with `exec_runtime()`:
    /// that starts at zero and is not scaled by the weight.
    [[nodiscard]]
    FORCE_INLINE
    auto vruntime() const -> SchedDuration {
        return m_fair.vruntime;
    }

    /// Execution time charged to this thread so far -- real time on a CPU, not
    /// any discipline's virtual time.
    ///
    /// The `exec_` is load bearing. Plain `runtime()` used to sit beside a
    /// per-CPU object called `Runtime` that had nothing to do with it, and it
    /// still sits beside Fair's `vruntime()`, which is the same quantity
    /// measured in virtual time. The two are not comparable: this one starts at
    /// zero and is not scaled by the weight, so arithmetic on Fair's virtual
    /// clock reads `vruntime()`. The name at least stops the framework from
    /// implying they are interchangeable.
    FORCE_INLINE
    auto exec_runtime() const -> SchedDuration {
        return m_exec_runtime;
    }

    /// What is left of the slice that started when this thread was last
    /// picked: the slice's length less the time charged since, never below
    /// zero. When it ends is this CPU's preemption deadline
    /// (Scheduler::set_next_thread()).
    [[nodiscard]]
    FORCE_INLINE
    auto remaining_slice() const -> SchedDuration {
        auto const left = m_timeslice.length() - (m_exec_runtime - m_prev_exec_runtime);
        if (left <= SchedDuration{0}) {
            return SchedDuration{0};
        }
        return left;
    }

    FORCE_INLINE
    auto weight() -> Weight {
        return m_profile.effective.weight;
    }

    FORCE_INLINE
    auto metric() -> Metric & {
        return m_metric;
    }

    /// The CPU this thread last ran on, or INVALID_CPU_NUM if it never has.
    /// Written when it leaves a CPU, in Scheduler::put_prev_thread().
    FORCE_INLINE
    auto last_cpu() const -> CpuNum {
        return m_last_cpu;
    }

    /// The CPU whose scheduler owns this thread -- running on it or queued on
    /// it -- or INVALID_CPU_NUM if no scheduler holds it. Written by
    /// Scheduler::enqueue_thread() and Scheduler::set_next_thread(), the two
    /// ways a thread comes to belong to a run queue.
    FORCE_INLINE
    auto curr_cpu() const -> CpuNum {
        return m_curr_cpu;
    }

    FORCE_INLINE
    auto runqueue_state() -> RunQueueState & {
        return m_runqueue_state;
    }

    FORCE_INLINE
    auto cpu_affinity() -> CpuAffinity & {
        return m_affinity;
    }

    template <typename NodeTraits>
    struct WaitQueueTraits;

  private:
    friend class Scheduler;
    friend class Fair;
    friend class RoundRobin;
    friend class task::OwnedWaitQueue;

    CpuAffinity m_affinity;
    RunQueueState m_runqueue_state;
    ustl::collections::intrusive::SetMemberHook<> m_runqueue_hook;

    /// The round-robin discipline's FIFO membership. A second hook rather
    /// than a reuse of m_runqueue_hook: that one is a set node for Fair's
    /// ordered RunQueue, and a list and a set do not share a node shape. A
    /// context is on at most one discipline's queue at a time, so the two are
    /// never both linked.
    ustl::collections::intrusive::ListMemberHook<> m_rr_hook;

    Metric m_metric;

    Profile m_profile;
    CpuNum m_last_cpu;
    CpuNum m_curr_cpu;
    CpuNum m_wake_cpu;

    /// The framework's own accounting: how long this thread has run, and where
    /// the current slice started counting from. Every discipline charges time
    /// the same way -- `RunQueue::charge_current()` hands back the interval --
    /// so these belong to the framework rather than to any one discipline.
    SchedDuration m_exec_runtime;
    SchedDuration m_prev_exec_runtime;

    /// One sub-struct per discipline that has private per-thread state.
    ///
    /// RoundRobin has none: its only per-thread datum is its queue hook, and
    /// that has to stay a direct member of this class because an intrusive
    /// container needs a pointer-to-member and there is no way to spell one
    /// that reaches into a nested struct. Moving the hooks in as well means
    /// the queues would have to hold the sub-struct and recover the entity
    /// from it -- a change to each discipline's own run queue, so it is
    /// theirs to make.
    FairState m_fair;

    Timeslice m_timeslice;

  public:
    USTL_DECLARE_HOOK_OPTION(Self, m_runqueue_hook, RunQueueOption);
    USTL_DECLARE_HOOK_OPTION(Self, m_rr_hook, RrQueueOption);

    template <typename = void>
    using RunQueueTraits =
        ustl::collections::intrusive::Mhtraits<Self, decltype(Self::m_runqueue_hook),
                                               &Self::m_runqueue_hook>;
};

template <typename NodeTraits>
struct SchedulerState::WaitQueueTraits {};

} // namespace ours::task

#endif // OURS_TASK_SCHEDULER_STATE_HPP
