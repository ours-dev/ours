/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_HPP
#define OURS_TASK_SCHED_SCHEDULER_HPP 1

#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/platform/timer.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/sched/config.hpp>
#include <ours/task/sched/rr.hpp>
#include <ours/task/sched/run_queue.hpp>
#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/types.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/timer.hpp>

#include <array>
#include <atomic>
#include <utility> // std::to_underlying

#include <gktl/canary.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/dyn.hpp>

namespace ours::task::sched {
#if SCHED_NORMAL_DISCIPLINE_FAIR
/// The reference to the Fair discipline, made in fair.cpp where the class is
/// complete.
auto fair_schedule() -> ustl::Ref<Schedule>;
#endif

/// The core scheduler per cpu.
class Scheduler {
    using Self = Scheduler;

  public:
    /// Scheduling mode.
    // [[=derive(Debug)]]
    enum class Mode {
        Idle,
        Regular,
        Preempt,
    };

    /// Scheduler state
    // [[=derive(Debug)]]
    enum class State {
        Idling,
        Active,
        Count,
    };

    class Pi;
    class Current;

    /// First thing a freshly spawned thread must do. It is the far end of
    /// the hand-off described at switch_context(): it releases the PI lock
    /// and the runqueue state that reschedule_common() left for it on some
    /// other stack, so nothing it releases was acquired in this function.
    TA_NO_THREAD_SAFETY_ANALYSIS
    static auto trampoline_handoff() -> void;

    /// |target_cpu| pins the thread instead of selecting for it; see the note
    /// on Thread::spawn_etc(), which is where it comes from.
    static auto on_thread_init(Thread *thread, BaseProfile const &profile,
                               CpuNum target_cpu = INVALID_CPU_NUM) -> void;

    /// Adopts |thread| as the one already running on |cpunum|'s scheduler.
    ///
    /// A scheduler's notion of "the current thread" is only ever written by a
    /// context switch, so the thread a CPU is executing *before* its first
    /// switch -- the boot context on the BSP, each CPU's idler once
    /// task::init() has built it -- has to be installed by hand, or
    /// m_run_queue.m_curr_thread stays null until something dereferences it.
    /// This pins the thread to its CPU, binds it to its discipline and makes
    /// it the run queue's current thread.
    static auto init_first_thread(Thread *thread, CpuNum cpunum) -> void;

    static auto on_thread_dead(Thread *thread) -> void;

    /// The caller holds |thread|'s PI lock and keeps it: this neither takes
    /// nor drops it, it only puts the thread on a runqueue. It used to be
    /// declared TA_ACQUIRE(thread->get_pi_lock()), and two callers in
    /// Thread::suspend_or_kill() took that literally -- they unguard()ed their
    /// Guard first, on the theory that this call would release the lock for
    /// them, and so leaked it.
    TA_REQUIRES(thread->get_pi_lock())
    static auto unpark_thread(Thread *thread, UnparkOptions = UnparkOptions::None) -> void;

    static auto unpark_thread(Thread::UnparkList, UnparkOptions = UnparkOptions::None) -> void;

    // TODO(SmallHuaZi)
    static auto migrate_thread(Thread *thread) -> void;

    // TODO(SmallHuaZi)
    static auto set_cpu_affinity(Thread *thread, CpuMask const &affinity, CpuAffinity::Kind kind)
        -> void;

    // TODO(SmallHuaZi)
    static auto on_thread_base_profile_overrided(Thread &thread) -> void;

    // TODO(SmallHuaZi)
    template <typename Target>
    static auto on_upstream_thread_base_profile_overrided(Thread &, Target &) -> void;

    // TODO(SmallHuaZi)
    template <typename Upstream, typename Target>
    static auto join_node_to_pi_graph(Upstream &, Target &) -> void;

    // TODO(SmallHuaZi)
    template <typename Upstream, typename Target>
    static auto split_node_from_pi_graph(Upstream &, Target &) -> void;

    FORCE_INLINE
    static auto peek_active_mask() -> CpuMask {
        return STATES[usize(State::Active)].load(std::memory_order_relaxed);
    }

    /// |cpunum| is the CPU this scheduler belongs to, which is not the CPU
    /// running this; see the definition for why it cannot be Cpu::id().
    INIT_CODE
    auto init(CpuNum cpunum) -> void;

    /// The Scheduler that owns |rq|.
    ///
    /// A RunQueue is a member of exactly one Scheduler, so this is the same
    /// member-to-owner recovery Thread::of(SchedulerState *) performs, and it is here
    /// for the same reason: a discipline's entry point is handed the RunQueue as
    /// its capability token and has no other honest way back to the scheduler
    /// around it.
    ///
    /// The disciplines used to spell it `Scheduler::Current::get(cpunum)`,
    /// which walks CPU_OFFSETS with a remote CPU number -- a load off an
    /// unrelated cacheline on every enqueue, tick and context switch, and a
    /// dependency on `which_cpu` being right for something the pointer
    FORCE_INLINE
    auto num_runnable() const -> usize {
        return m_run_queue.m_num_queued_threads;
    }

    /// The weight of the threads this CPU holds, the one it runs counted:
    /// num_runnable() weighed. Read from another CPU it is a hint, as the count
    /// is, and exact under this CPU's run queue lock.
    [[nodiscard]]
    FORCE_INLINE
    auto load() const -> Weight {
        return m_run_queue.m_total_weight;
    }

    /// Makes this CPU take queued threads from the CPUs of |victims|: as many
    /// as bring it up to the mean load of the CPUs it looks at, and no more than
    /// the victim holds above that mean. This is the scheduler's one way of
    /// taking work from another CPU, and it may be called on any CPU's
    /// scheduler, by any CPU: the idle thread calls it on its own with every
    /// online CPU, and a balancer on one CPU can call it on another's, to bring
    /// the machine to a balance. |victims| narrows what is looked at, the mean
    /// included. Answers how many threads it moved here, none when stealing is
    /// off (StealPolicy), when this CPU is not active, or when nothing is worth
    /// taking.
    ///
    /// The tiers of this CPU's scheduling domains are tried nearest first
    /// (domains_of()); in each, the victim is the CPU with the most load among
    /// those that hold as many threads as the policy asks for a tier of its
    /// kind, the one they run counted. The mean is over this CPU and every CPU
    /// of the tiers up to that one. For a CPU that has nothing to run, the first
    /// thread is taken whatever it weighs, even when the mean is not above the
    /// victim; every thread of a CPU that is running something must fit what is
    /// left of the mean. The others fit as well, and no more than the tier's
    /// batch are taken, among the first sched_domain::MAX_SCAN of the victim's
    /// queue. The threads have to be ones that may run here.
    ///
    /// The victim and the mean are picked from the CPUs' loads, read without
    /// their locks, so they are a hint that the victim's own load, read under
    /// its lock, confirms before anything is taken. The move is two steps, each
    /// under one run queue lock and never both, as deactivate() does it: the
    /// threads leave the victim's queue (Migrating), then join this one. No
    /// lock is held on entry, and the call holds preemption off throughout, so
    /// a caller that may be preempted is not switched out between the steps. The
    /// caller's CPU is not assumed to be this one: a CPU other than the caller's
    /// that has taken threads is asked to reschedule, so that one that is halted
    /// looks at its queue; the caller's own is not, and a caller that is the
    /// idle loop acts on the answer. A thief that stops being active between the
    /// two steps (it is going offline) hands the threads to active CPUs
    /// instead, as deactivate() does.
    ///
    /// Two calls at once for one thief are not serialised, and together can take
    /// up to twice what the mean calls for (item 36 of
    /// docs/repairs/261103/kernel-task-sched.md): the caller sees to it.
    ///
    /// Not in pick_next_thread(), where the pick holds this CPU's run queue lock
    /// and a take needs the victim's: with no order of the two defined, two CPUs
    /// taking from each other would deadlock, and a CPU that comes back online
    /// enters the idle loop without picking (the user's call, 2026-11-05).
    [[nodiscard]]
    TA_EXCLUDES(get_lock()) auto steal_work_from(CpuMask const &victims) -> usize;

    FORCE_INLINE
    auto is_state_active() const -> bool {
        return STATES[usize(State::Active)]
            .load(std::memory_order_relaxed)
            .test(m_run_queue.m_which_cpu);
    }

    /// TA_RETURN_CAPABILITY makes `get_lock()` and `m_run_queue.m_rqlock` one
    /// capability to the analysis. Both spellings are in use: Scheduler's own
    /// methods say `TA_REQUIRES(get_lock())`, a discipline's entry points say
    /// `TA_REQUIRES(rq.rqlock)`, and enqueue_thread() calls the latter
    /// while holding the former.
    TA_RETURN_CAPABILITY(m_run_queue.m_rqlock) FORCE_INLINE auto get_lock() -> sync::SpinLock & {
        return m_run_queue.m_rqlock;
    }

  private:
    class Migrator;

    auto assert_thread_in_scheduler(Thread *thread) -> void;

    static auto reschedule(CpuNum cpu) -> void;

    static auto reschedule_mask(CpuMask const &mask) -> void;

    /// Ask the discipline for a CPU, then fall back to the affinity and load checks.
    static auto select_target_cpu(Thread *thread) -> CpuNum;

    /// The core internal scheduling routine. Regardless whether IRQ and preemption was either
    /// disabled or not, it mainly do the following things about scheduling behaviour itself:
    ///     1) Acquires the lock of runqueue.
    ///     2) Pick next ready thread.
    ///     3) Drop the lock held itself.
    ///     4) Switch to the thread selected.
    ///     5) Drop the |curr|'s lock.
    ///
    /// Assumptions:
    ///     1) |curr|'s lock was held.
    ///     2) Local interrupts was disabled.
    ///     3) Preemption was disabled.
    ///
    /// The annotations say the same as the list above: the caller holds
    /// |curr|'s PI lock and must *not* hold this scheduler's runqueue lock,
    /// because step 1 takes it here and step 3 hands it to switch_context().
    /// This used to read TA_REQUIRE(get_lock()), the exact opposite of step 1,
    /// and that inverted contract is what let Current::yield_thread() call in
    /// with the runqueue lock already held -- a self-deadlock on a
    /// non-reentrant SpinLock that the analysis could not report while it
    /// was being told the lock was supposed to be held.
    TA_REQUIRES(curr->get_pi_lock())
    TA_EXCLUDES(get_lock())
    auto reschedule_common(Thread *curr, Mode mode) -> void;

    /// Declared, not yet defined. Whatever it ends up doing, the runqueue
    /// lock is this class's to take internally, never the caller's to hand in
    /// or receive back -- so no lock annotation until there is a body to
    /// describe.
    auto unpark_common(Thread *curr, Mode mode) -> void;

    TA_REQUIRES(get_lock())
    auto update_runtime() -> void;

    /// Drop the current scheduler's lock, and then switch context from |curr|'s to |next|'s.
    ///
    /// Lock hand-off, which the analysis cannot express, so both of these
    /// are opted out of it rather than annotated wrongly:
    ///
    ///  * The runqueue lock is held on entry -- reschedule_common() took it
    ///    with a Guard and unguard()ed that Guard so it will not release --
    ///    and is released here, after prev/curr are recorded, before the
    ///    stack switch. That is a plain release the analysis could follow, but
    ///    unguard() already counts as one, so TA_RELEASE here reported a
    ///    double release at every call site.
    ///
    ///  * |curr|'s PI lock is held on entry and *not* released by this
    ///    activation. finish_switch_context() releases the PI lock of
    ///    whichever thread ran before -- the one recorded in
    ///    m_run_queue.m_prev_thread -- on the far side of the switch, on the
    ///    next thread's stack. A lock released by a different activation than
    ///    the one that acquired it is outside the analysis's model entirely.
    ///
    /// The contract that is checkable -- callers hold |curr|'s PI lock -- is
    /// on reschedule_common().
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto switch_context(Thread *curr, Thread *next) -> void;

    /// Release the outgoing thread's PI lock on the CPU that resumed us.
    TA_NO_THREAD_SAFETY_ANALYSIS
    static auto complete_context_switch() -> void;

    TA_REQUIRES(get_lock())
    auto enqueue_thread(Thread *thread, EnqueueOptions options) -> Status;

    TA_REQUIRES(get_lock())
    auto dequeue_thread(Thread *thread, DequeueOptions options) -> Status;

    /// Activate the given |thread|. That means to add it onto runqueue and
    /// mark the thread ready to be scheduled.
    ///
    /// Both are runqueue mutations -- they wrap enqueue_thread()/
    /// dequeue_thread(), which carry the same requirement -- so the caller
    /// holds this scheduler's runqueue lock.
    TA_REQUIRES(get_lock())
    auto activate_thread(Thread *thread, EnqueueOptions options) -> void;

    /// Deactivate the given |thread|. That means to remove it from runqueue and
    /// disallow it to be added onto runqueue.
    TA_REQUIRES(get_lock())
    auto deactivate_thread(Thread *thread, DequeueOptions options) -> void;

    /// Unpark the given |thread| with specified flags |control| on this scheduler.
    ///
    /// The caller holds |thread|'s PI lock and keeps it -- this does not drop
    /// it, whatever the previous comment said. The runqueue lock is taken and
    /// released inside, in one Guard scope, which is why it is TA_EXCLUDES
    /// and not TA_ACQUIRE: the caller must come in without it and leaves
    /// without it. (TA_ACQUIRE would mean "held on return", the contract of
    /// a lock() function, and the analysis duly reported the Guard's release
    /// as "expecting lock to be held at the end of function".)
    TA_REQUIRES(thread->get_pi_lock())
    TA_EXCLUDES(get_lock())
    auto do_unpark_thread(Thread *thread, UnparkOptions options) -> void;

    /// Evaulates the given |thread| whether be able to preempt current thread.
    /// If it can does, send out preemption request to this scheduler.
    TA_REQUIRES(get_lock())
    auto unpark_preempt(Thread *thread, UnparkOptions options) -> void;

    /// Which thread should run next on this CPU. Pure selection: it reads the
    /// run queues and answers, and changes nothing about who owns the CPU.
    ///
    /// Under multi-processor environment it will attempt to steal a task from
    /// other remote CPUs. It never returns null -- the idle thread is the
    /// answer when nothing else is runnable.
    TA_REQUIRES(get_lock())
    auto pick_next_thread(Thread *curr) -> Thread *;

    /// Picks the next thread and hands the CPU over to it: |curr| is put back
    /// and |next| is taken up.
    ///
    /// The two halves used to be one function, with the hand-over in a
    /// gktl::defer at the top so that it ran on every `return` in the middle.
    /// Splitting them is not cosmetic. The defer captured the result by
    /// reference from a variable that started out uninitialised, so "did every
    /// path write it before returning?" was a question a reader had to answer
    /// by checking each `return` -- and one path had already got it wrong
    /// once. It is also invisible to the thread-safety analysis, which treats
    /// a lambda body as its own function with an empty lockset, so the two
    /// most-run mutators of the run queue could never carry a lock contract
    /// while they were reached from inside it.
    TA_REQUIRES(get_lock())
    auto evaluate_next_thread(Thread *curr) -> Thread *;

    /// [MP] Takes migrating_threads_lock only long enough to swap the list
    /// out, then works on the private copy unlocked. Same TA_EXCLUDES logic as
    /// do_unpark_thread(): the lock is acquired and released within.
    TA_EXCLUDES(m_run_queue.m_migrating_threads_lock)
    auto process_migrating_threads(SchedTime now) -> void;

    /// A stub (asserts only) called from process_migrating_threads() under
    /// the *target* scheduler's runqueue lock and no PI lock. It used to
    /// claim TA_REQUIRES(thread->get_pi_lock()), which that caller does not
    /// satisfy; as with unpark_common(), the lock contract waits for the
    /// body -- annotating a stub only records a guess.
    auto finish_transition(Thread *thread) -> void;

    /// Flips this CPU's bit in STATES[state]. Lock-free: STATES is global
    /// and every CPU updates its own bit in the same word, so this CPU's
    /// runqueue lock could never have protected it -- the TA_REQUIRES(get_lock())
    /// it used to carry promised an exclusion that did not exist, and made
    /// Current::set_state() take the lock for nothing.
    auto set_state(State state, bool on) -> void;

    TA_REQUIRES(get_lock())
    auto put_prev_thread(Thread *thread) -> void;

    TA_REQUIRES(get_lock())
    auto set_next_thread(Thread *thread) -> void;

    gktl::Canary m_canary{this};
    RunQueue m_run_queue;
    usize m_num_switches;

    Thread *m_idle_thread;

    static constexpr auto NUM_STATE = std::to_underlying(State::Count);
    static inline std::array<std::atomic<CpuMask>, NUM_STATE> STATES{};
};

/// This helper class was designed for conviencies and securities.
class Scheduler::Current: public mm::CpuLocal::Current<Scheduler> {
  public:
    /// Reschedule at IDLE mode
    TA_REQUIRES(curr->get_pi_lock())
    static auto idle(Thread *curr) -> void;

    /// Reschedule at Preempt mode
    TA_REQUIRES(curr->get_pi_lock())
    static auto preempt(Thread *curr) -> void;

    /// Reschedule at Regular mode
    TA_REQUIRES(curr->get_pi_lock())
    static auto reschedule(Thread *curr) -> void;

    TA_REQUIRES(curr->get_pi_lock())
    static auto yield_thread(Thread *curr) -> void;

    TA_REQUIRES(curr->get_pi_lock())
    static auto yield_to_thread(Thread *curr, Thread *next) -> Status;

    /// Called when a thread is forced to wait, e.g. waiting for a mutex or a signal.
    /// Mainly used by WaitQueue.
    ///
    TA_REQUIRES(curr->get_pi_lock())
    static auto park_thread(Thread *curr) -> void;

    /// This CPU's preemption deadline has passed: the running thread's slice
    /// is over. Asks for a reschedule at the next preemption point, the exit
    /// of the timer interrupt this is called from
    /// (TimerQueue::run_expired_timers()); the reschedule rotates the thread
    /// and sets the next deadline (set_next_thread()).
    static auto on_preemption_deadline() -> void;

    static auto set_active(bool on) -> void {
        Current::set_state(State::Active, on);
    }

    /// This CPU is going offline. Called by its idle thread, the running
    /// thread, with interrupts off: clears the CPU's Active bit, under its
    /// run queue lock, so that no thread is placed here from now on, then
    /// moves every queued thread that may run on another active CPU to one.
    /// A thread that may run only here stays queued until the CPU is back;
    /// one that wakes meanwhile has nowhere to go, and selection asserts.
    ///
    /// The threads are found by walking the run queue
    /// (RunQueue::for_each_thread()) and moved through their discipline's
    /// dequeue, so a thread that stays is never touched.
    static auto deactivate() -> void;

    /// The CPU is back: Active again, under its run queue lock. On the CPU,
    /// interrupts off.
    static auto reactivate() -> void;

    /// How many times |cpu| has looked for threads to take (calls of
    /// steal_work_from() on its scheduler, whoever made them), and how many
    /// threads it took in all, for a test to tell a CPU that looked and found
    /// nothing from one that never looked. Read while |cpu| may be counting.
    [[nodiscard]]
    static auto num_steal_looks(CpuNum cpu) -> u64;
    [[nodiscard]]
    static auto num_steals(CpuNum cpu) -> u64;

    /// How long calls of steal_work_from() on |cpu|'s scheduler have taken in
    /// all, and the longest one, in nanoseconds of the steady clock, whoever
    /// made them: the time the call held the victim's lock with interrupts off
    /// is part of it. Each reading includes about one read of
    /// the clock, and an emulator's clock is slow, so the figures are
    /// comparisons between settings on one machine, not times a machine would
    /// give. Calls with stealing off are not counted.
    [[nodiscard]]
    static auto steal_nanoseconds(CpuNum cpu) -> u64;
    [[nodiscard]]
    static auto longest_steal_nanoseconds(CpuNum cpu) -> u64;

  private:
    friend class Thread;
    friend class Thread::Current;

    static auto set_idling(bool on) -> void {
        Current::set_state(State::Idling, on);
    }

    static auto set_state(State state, bool on) -> void {
        auto &self = Scheduler::Current::get();
        self.m_canary.verify<Self>();
        self.set_state(state, on);
    }
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULER_HPP
