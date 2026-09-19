/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_HPP
#define OURS_TASK_SCHED_SCHEDULER_HPP 1

#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/platform/timer.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/sched/config.hpp>
#include <ours/task/sched/fair.hpp>
#include <ours/task/sched/rr.hpp>
#include <ours/task/sched/runtime.hpp>
#include <ours/task/sched/types.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/timer.hpp>

#include <array>
#include <atomic>
#include <utility> // std::to_underlying

#include <gktl/canary.hpp>
#include <ustl/chrono/duration.hpp>

namespace ours::task::sched {
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
    /// m_runtime.curr_thread stays null until something dereferences it.
    /// Zircon does the same in Scheduler::InitializeFirstThread(): mark the
    /// thread running, pin it to its CPU, make it active_thread_.
    static auto init_first_thread(Thread *thread, CpuNum cpunum) -> void;

    static auto on_thread_dead(Thread *thread) -> void;

    /// The caller holds |thread|'s PI lock and keeps it: this neither takes
    /// nor drops it, it only puts the thread on a runqueue (Zircon's
    /// Scheduler::Unblock has the same TA_REQ(thread_lock) shape). It used
    /// to be declared TA_ACQUIRE(thread->get_pi_lock()), and two callers in
    /// Thread::suspend_or_kill() took that literally -- they unguard()ed
    /// their Guard first, on the theory that this call would release the
    /// lock for them, and so leaked it.
    TA_REQUIRES(thread->get_pi_lock())
    static auto unpark_thread(Thread *thread, UnparkControl = UnparkControl::None) -> void;

    static auto unpark_thread(Thread::UnparkList, UnparkControl = UnparkControl::None) -> void;

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

    auto assert_thread_in_scheduler(Thread *thread) -> void;

    /// The Scheduler that owns |runtime|.
    ///
    /// A Runtime is a member of exactly one Scheduler, so this is the same
    /// member-to-owner recovery Thread::of(SchedEntity *) performs, and it is here
    /// for the same reason: an ISchedule entry point is handed the Runtime as
    /// its capability token and has no other honest way back to the scheduler
    /// around it.
    ///
    /// The disciplines used to spell it `Scheduler::Current::get(cpunum)`,
    /// which walks CPU_OFFSETS with a remote CPU number -- a load off an
    /// unrelated cacheline on every enqueue, tick and context switch, and a
    /// dependency on `which_cpu` being right for something the pointer
    /// arithmetic already knows. This is a compile-time constant subtraction.
    FORCE_INLINE
    static auto of(Runtime &runtime) -> Self & {
        return *ustl::mem::container_of(&runtime, &Self::m_runtime);
    }

    FORCE_INLINE
    auto num_runnable() const -> usize {
        return m_runtime.num_queued_threads;
    }

    FORCE_INLINE
    auto is_state_active() const -> bool {
        return STATES[usize(State::Active)]
            .load(std::memory_order_relaxed)
            .test(m_runtime.which_cpu);
    }

    /// TA_RETURN_CAPABILITY makes `get_lock()` and `m_runtime.rqlock` one
    /// capability to the analysis. Both spellings are in use: Scheduler's own
    /// methods say `TA_REQUIRES(get_lock())`, ISchedule's virtuals say
    /// `TA_REQUIRES(runtime.rqlock)`, and enqueue_thread() calls the latter
    /// while holding the former.
    TA_RETURN_CAPABILITY(m_runtime.rqlock) FORCE_INLINE auto get_lock() -> sync::SpinLock & {
        return m_runtime.rqlock;
    }

    /// The Fair discipline's run queue for this CPU. Held directly here
    /// (rather than through its own CpuLocal instance) since Scheduler
    /// is already a per-CPU singleton -- see RunQueue's doc comment in
    /// fair.hpp for why this deliberately couples Scheduler to Fair.
    FORCE_INLINE
    auto fair_run_queue() -> RunQueue & {
        return m_fair_runqueue;
    }

    /// The round-robin discipline's FIFO for this CPU, held here for the same
    /// reason as the Fair queue above. Both slots exist at once so that
    /// switching disciplines is a matter of which one is registered, not of
    /// which one Scheduler was compiled to carry.
    FORCE_INLINE
    auto rr_run_queue() -> RrRunQueue & {
        return m_rr_runqueue;
    }

  private:
    class Migrator;

    /// [MP]
    template <typename F, typename... Args>
    static auto find_active_scheduler_for(Thread *thread, F f, Args &&...args) -> CpuNum;

    static auto reschedule(CpuNum cpu) -> void;

    static auto reschedule_mask(CpuMask const &mask) -> void;

    /// Assign a CPU on which the given |thread| will run.
    /// It first attempt to use specific method |IScheduler::select_target_cpu|.
    /// If got a invalid CPU number, it will adopts fallback method.
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
    ///    m_runtime.prev_thread -- on the far side of the switch, on the
    ///    next thread's stack. A lock released by a different activation than
    ///    the one that acquired it is outside the analysis's model entirely.
    ///
    /// Zircon opts its equivalents out the same way (Scheduler::
    /// FinalContextSwitch and Thread::Current::Reschedule are
    /// __TA_NO_THREAD_SAFETY_ANALYSIS around the thread_lock hand-off).
    /// The contract that is checkable -- callers hold |curr|'s PI lock -- is
    /// on reschedule_common().
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto switch_context(Thread *curr, Thread *next) -> void;

    /// The far side of switch_context(): releases the PI lock of whichever
    /// thread ran before, and settles this CPU's idle state.
    ///
    /// |curr| is the thread running *now*, not the one being switched to --
    /// both call sites pass the resuming thread (switch_context() passes its
    /// own |curr|, trampoline_handoff() passes m_runtime.curr_thread), and
    /// the definition has always named it so. The declaration said |next|,
    /// which is the mental model from before this became a static: back then
    /// the caller was on the near side of the switch and the thread it named
    /// was indeed the one about to run. On this side of the switch that
    /// thread is the one running, so the name has to move with it.
    ///
    /// Static, and deliberately so: it reaches Current::get() itself rather
    /// than taking a `this`, because the scheduler that finishes a switch is
    /// the one belonging to whichever CPU is running the thread now -- see
    /// the paragraph on switch_context() above.
    TA_NO_THREAD_SAFETY_ANALYSIS
    static auto complete_context_switch(Thread *curr) -> void;

    TA_REQUIRES(get_lock())
    auto enqueue_thread(Thread *thread, EnqueueOptions options) -> Status;

    TA_REQUIRES(get_lock())
    auto dequeue_thread(Thread *thread, DequeueOptions options) -> Status;

    /// The routine called in timer interrupt routine.
    auto tick_thread(Thread *current, SchedTime now) -> void;

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
    auto do_unpark_thread(Thread *thread, UnparkControl control) -> void;

    /// Evaulates the given |thread| whether be able to preempt current thread.
    /// If it can does, send out preemption request to this scheduler.
    TA_REQUIRES(get_lock())
    auto unpark_preempt(Thread *thread, UnparkControl control) -> void;

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
    ///
    /// Linux splits the same seam the same way: pick_task() chooses, and
    /// put_prev_set_next_task() performs the hand-over.
    TA_REQUIRES(get_lock())
    auto evaluate_next_thread(Thread *curr) -> Thread *;

    auto set_schedule(Thread *thread) -> ISchedule *;

    auto get_schedule(Thread *thread) -> ISchedule *;

    /// [MP] Attempts to steal work from other busy CPUs spcified by |mask| and move it
    /// to the local run queues. Returns a pointer to the stolen thread that is now
    /// associated with the local Scheduler instance, or nullptr is no work was stolen.
    auto steal_work_from(CpuMask const &mask) -> Thread *;

    /// [MP] Takes migrating_threads_lock only long enough to swap the list
    /// out, then works on the private copy unlocked. Same TA_EXCLUDES logic as
    /// do_unpark_thread(): the lock is acquired and released within.
    TA_EXCLUDES(m_runtime.migrating_threads_lock)
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
    Runtime m_runtime;
    usize m_num_switches;

    /// Indexed directly by `Discipline` -- entry `i` is the schedule
    /// implementing `Discipline(i)`, or null if none is registered.
    static constexpr auto NUM_DISCIPLINE = std::to_underlying(Discipline::Count);
    std::array<ISchedule *, NUM_DISCIPLINE> m_schedules{};

    RunQueue m_fair_runqueue;
    RrRunQueue m_rr_runqueue;

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

    static auto tick(SchedTime now) -> void;

    static auto set_active(bool on) -> void {
        Current::set_state(State::Active, on);
    }

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
