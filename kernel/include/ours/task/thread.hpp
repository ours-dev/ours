/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_THREAD_HPP
#define OURS_TASK_THREAD_HPP 1

#if !__has_include(<ours/arch/thread.hpp>)
#error "The header <ours/arch/thread.hpp> is required by [protocol.module.task]"
#endif
#include <ours/arch/thread.hpp>
#include <ours/compiler.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/memory_allocation_state.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/stack.hpp>
#include <ours/mm/types.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/config.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/task/preemption_state.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/types.hpp>
#include <ours/task/wait_queue.hpp>

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ktl/rwonce.hpp>
#include <lockdep/lock_context.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/container_of.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours {
namespace object {
    class ProcessDispatcher;
    class ThreadDispatcher;
} // namespace object

namespace task {
    using sched::DEFAULT_PRIORITY;
    using sched::HIGHEST_PRIORITY;
    using sched::LOWEST_PRIORITY;

    class TaskState {
        using Self = TaskState;

      public:
        FORCE_INLINE
        auto init(ThreadStartRoutine entry, void *arg) -> void {
            m_entry_point = entry;
            m_arg = arg;
            m_retcode = 0;
        }

        FORCE_INLINE
        auto invoke() -> i32 {
            DEBUG_ASSERT(m_entry_point);
            m_retcode = m_entry_point(m_arg);
            m_result_wait_queue.notify_all(Status::Ok);
            return m_retcode;
        }

        FORCE_INLINE
        auto retcode() const -> i32 {
            return m_retcode;
        }

        FORCE_INLINE
        auto set_retcode(i32 retcode) -> void {
            m_retcode = retcode;
        }

        auto join(Thread *thread, time::Deadline) -> Status;

        auto try_wake_joiners(Status status) -> bool;

      private:
        i32 m_retcode;
        ThreadStartRoutine m_entry_point;
        void *m_arg;
        WaitQueue m_result_wait_queue;
    };

    class Thread {
        using Self = Thread;

      public:
        enum class State : u16 {
            /// The unique states
            Running = 0_bit,
            Ready = 1_bit,
            Dead = 2_bit,

            Suspend = 3_bit,
            Initial = 4_bit,

            Parked = 5_bit,
            Waking = 6_bit,
            Sleeping = 7_bit,

            Special = Parked | Sleeping | Suspend,
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(State);

        friend auto to_string(Thread::State state) -> char const *;

        using Name = ktl::Name<32>;
        using TrampolineRoutine = auto (*)() -> void;
        using StartRoutine = ThreadStartRoutine;

        class Current;

        /// Creates a thread with `name` that will execute `entry` at `priority`. |arg|
        /// will be passed to `entry` when executed, the return value of `entry` will be
        /// passed to Exit().
        ///
        /// This call allocates a thread and places it in the global thread list. This
        /// memory will be freed by either Join() or Detach(), one of these must be called.
        /// The thread will not be scheduled until resume() is called.
        /// |arg| is handed to |entry| when the thread runs, and is the only
        /// context it gets. The variadic overloads that used to sit here bound
        /// their arguments into a std::function; see ThreadStartRoutine for
        /// why that could not stay. A caller with a non-static member function
        /// or an extra argument wraps it in a captureless lambda, which
        /// converts to a plain function pointer and allocates nothing.
        static auto spawn(char const *, StartRoutine, void *arg, sched::BaseProfile const &)
            -> Self *;

        static auto spawn(char const *, StartRoutine, void *arg, usize priority) -> Self *;

        /// |target_cpu| pins the new thread to one CPU instead of letting the
        /// scheduler choose. INVALID_CPU_NUM, the default, means "no opinion"
        /// and is what an ordinary thread passes.
        ///
        /// It exists for threads whose CPU is a property of what they are
        /// rather than a placement decision -- the idlers, one per CPU, whose
        /// number IdleThread::init() already knows. Sending those through
        /// generic selection is not merely redundant: selection can only
        /// answer with a CPU that is online, and task::init() builds every
        /// idler while walking cpu_possible_mask(), long before the secondary
        /// CPUs come up. The answer for a CPU that is not online yet has to
        /// come from the caller, because there is nothing for selection to
        /// read.
        static auto spawn_etc(KoId, char const *, StartRoutine, void *arg,
                              sched::BaseProfile const &, TrampolineRoutine, Self *,
                              CpuNum target_cpu = INVALID_CPU_NUM) -> Status;

        /// Builds a thread around the execution context that is already running.
        ///
        /// The boot-path counterpart of spawn_etc(): no stack is allocated and no
        /// entry point is installed, because the caller is already executing on the
        /// stack the linker script provided and its context lives in the live
        /// registers. `Stack::init_first()` is what adopts that stack.
        ///
        /// This exists because spawn_etc() cannot run this early. It reaches
        /// Stack::init(), which needs both the slab (VmObjectPaged::create) and
        /// VmAspace::kernel_aspace(); neither is up until init_pmm() and init_vmm(),
        /// four steps after task::init_early(). Scheduler::on_thread_init() is
        /// likewise skipped: it does a CPU-indexed Scheduler::Current lookup, and
        /// CpuLocal has only installed the running CPU's offset at this point.
        ///
        /// The counterpart in Zircon is ConstructFirstThread().
        static auto create_first(KoId, char const *, sched::BaseProfile const &, Self *) -> void;

        /// Core method family.

        auto join(i32 *retcode, time::Deadline deadline) -> Status;

        auto detach() -> Status;

        /// Make the thread transition to runnable or running state.
        ///
        /// Paired with Thread::suspend.
        auto resume() -> Status;

        auto detach_and_resume() -> Status;

        auto suspend() -> Status {
            return suspend_or_kill(SuspendOrKillOp::Suspend);
        }

        auto kill() -> Status {
            return suspend_or_kill(SuspendOrKillOp::Kill);
        }

        /// Both take a copy of the mask, so a temporary built at the call
        /// site -- `CpuMask::of(n)` -- is fine. It was not always: the mask
        /// used to be stored by address.
        auto set_cpu_affinity_hard(CpuMask const &) -> void;

        auto set_cpu_affinity_soft(CpuMask const &) -> void;

        auto override_base_profile(sched::BaseProfile const &profile) -> void;

        auto bind_user_thread(ustl::Arc<object::ThreadDispatcher> user_thread) -> void;

        /// The user-mode thread this one is running on behalf of, or an empty
        /// Arc for an ordinary kernel thread.
        ///
        /// Read-only, and the only way out of this class to the object layer.
        /// Thread::Current::fault() has always made this walk, but from
        /// *inside* Thread, where the member is reachable; the syscall path
        /// needs the same answer from outside it -- it has to ask which
        /// process trapped in order to check the trap came from that process's
        /// vDSO.
        FORCE_INLINE
        auto user_thread() const -> ustl::Arc<object::ThreadDispatcher> const & {
            return m_user_thread;
        }

        /// The followings is a group of accessor.
        FORCE_INLINE
        auto tid() -> KoId {
            return m_tid;
        }

        FORCE_INLINE
        auto state() -> State {
            return m_state;
        }

        FORCE_INLINE
        auto signals() -> ThreadSignals {
            return m_signals;
        }

        FORCE_INLINE
        /// The address space this thread runs in, or null for a kernel
        /// thread -- Scheduler::switch_context() reads it on every switch and
        /// VmAspace::switch_aspace() reads null as "the kernel's".
        ///
        /// Set before the thread is first resumed. Changing it under a
        /// running thread would leave the CPU on a CR3 the scheduler no
        /// longer believes is loaded.
        auto set_aspace(mm::VmAspace *aspace) -> void {
            m_aspace = aspace;
        }

        auto aspace() -> mm::VmAspace * {
            return m_aspace;
        }

        FORCE_INLINE
        auto kernel_stack() -> mm::Stack & {
            return m_stack;
        }

        FORCE_INLINE
        auto last_cpu() const -> CpuNum {
            return m_scheduling_entity.last_cpu();
        }

        FORCE_INLINE
        auto curr_cpu() const -> CpuNum {
            return m_scheduling_entity.curr_cpu();
        }

        FORCE_INLINE
        auto is_state_dead() const -> bool {
            return m_state == State::Dead;
        }

        FORCE_INLINE
        auto is_state_initial() const -> bool {
            return m_state == State::Initial;
        }

        FORCE_INLINE
        auto is_state_ready() const -> bool {
            return m_state == State::Ready;
        }

        FORCE_INLINE
        auto is_state_sleeping() const -> bool {
            return m_state == State::Sleeping;
        }

        FORCE_INLINE
        auto is_state_parked() const -> bool {
            return m_state == State::Parked;
        }

        FORCE_INLINE
        auto is_state_running() const -> bool {
            return m_state == State::Running;
        }

        FORCE_INLINE
        auto is_state_suspend() const -> bool {
            return m_state == State::Suspend;
        }

        FORCE_INLINE
        auto is_state_special() const -> bool {
            return m_state == State::Special;
        }

        /// Reads m_flags without its lock, on purpose: Idle is set once,
        /// before the thread is published (IdleThread::init), and never
        /// changes, so there is nothing for the lock to order against. The
        /// callers are scheduler paths that hold the runqueue lock, not the
        /// PI lock, and asking them to take it for an immutable bit would be
        /// ceremony. Detached is the only bit of m_flags that moves.
        TA_NO_THREAD_SAFETY_ANALYSIS
        FORCE_INLINE
        auto is_idle_thread() const -> bool {
            return !!(m_flags & Flags::Idle);
        }

        FORCE_INLINE
        auto name() const -> char const * {
            return m_name.data();
        }

        /// Both accessors carry TA_RETURN_CAPABILITY so that the TA_GUARDED
        /// members below, which name the lock through the accessor, are seen
        /// as protected by the same capability a caller's Guard takes.
        TA_RETURN_CAPABILITY(m_resource_lock)
        FORCE_INLINE auto get_resource_lock() -> sync::SpinLock & {
            return m_resource_lock;
        }

        TA_RETURN_CAPABILITY(m_pi_lock) FORCE_INLINE auto get_pi_lock() -> sync::SpinLock & {
            return m_pi_lock;
        }

        FORCE_INLINE
        auto waiter_state() -> WaiterState & {
            return m_waiter_state;
        }

        FORCE_INLINE
        auto preemption_state() -> PreemptionState & {
            return m_preemption_state;
        }

        FORCE_INLINE
        auto memory_allocation_state() -> mm::MemoryAllocationState & {
            return m_memory_allocation_state;
        }

        FORCE_INLINE
        auto check_kill_signal() const -> bool {
            return !!(m_signals & ThreadSignals::Kill);
        }

        FORCE_INLINE
        auto check_kill_or_suspend_signal() const -> Status {
            if (!!(m_signals & ThreadSignals::Kill)) {
                return Status::InternalIrqKilled;
            } else if (!!(m_signals & ThreadSignals::Suspend)) {
                return Status::InternalIrqRetry;
            }

            return Status::Ok;
        }

      private:
        enum class Flags : u32 {
            Interruptible = 0_bit,
            SignalPending = 1_bit,
            Detached = 2_bit,

            /// I'm a idle thread
            Idle = 3_bit,

            Restricted = 4_bit,

            NoFree = 30_bit,
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

        static auto switch_thread(Self *curr, Self *next) -> void;

        FORCE_INLINE
        static auto of(sched::SchedEntity *se) -> Self * {
            return ustl::mem::container_of(se, &Self::m_scheduling_entity);
        }

        FORCE_INLINE
        static auto of(ArchThread *arch) -> Self * {
            return ustl::mem::container_of(arch, &Self::m_arch_thread);
        }

        FORCE_INLINE
        static auto of(WaiterState *waiter) -> Self * {
            return ustl::mem::container_of(waiter, &Self::m_waiter_state);
        }

        Thread(KoId tid, char const *name, sched::BaseProfile const &profile);

        ~Thread() = default;

        FORCE_INLINE
        auto scheduling_entity() -> sched::SchedEntity & {
            return m_scheduling_entity;
        }

        /// Migration shim for `fair.*`; see sched/context.hpp for why it is
        /// here and when to delete it. Nothing else calls this name.
        FORCE_INLINE
        auto scheduling_context() -> sched::SchedEntity & {
            return scheduling_entity();
        }

        FORCE_INLINE
        auto set_state_ready() -> void {
            ktl::write_once(m_state, State::Ready);
        }

        FORCE_INLINE
        auto set_state_running() -> void {
            ktl::write_once(m_state, State::Running);
        }

        FORCE_INLINE
        auto set_state_parked() -> void {
            ktl::write_once(m_state, State::Parked);
        }

        FORCE_INLINE
        auto set_state_dead() -> void {
            ktl::write_once(m_state, State::Dead);
        }

        FORCE_INLINE
        auto set_state_sleeping() -> void {
            ktl::write_once(m_state, State::Sleeping);
        }

        FORCE_INLINE
        auto set_state_suspend() -> void {
            ktl::write_once(m_state, State::Suspend);
        }

        TA_REQUIRES(get_pi_lock()) FORCE_INLINE auto set_flag_idle() -> void {
            m_flags |= Flags::Idle;
        }

        TA_REQUIRES(get_pi_lock()) FORCE_INLINE auto set_flag_nofree() -> void {
            m_flags |= Flags::NoFree;
        }

        // ObjectCache::Instance placement-news and destroys T; Thread's
        // constructor and destructor stay private otherwise.
        template <typename, typename...>
        friend class mm::ObjectCache::Instance;

        friend ArchThread;
        friend IdleThread;

        friend sched::Scheduler;
        friend sched::Fair;
        friend sched::RoundRobin;
        friend class object::ProcessDispatcher;

        friend WaitQueue;
        friend OwnedWaitQueue;

        [[noreturn]]
        static auto trampoline() -> void;

        enum class SuspendOrKillOp {
            Suspend,
            Kill,
        };
        auto suspend_or_kill(SuspendOrKillOp) -> Status;

        auto destroy() -> void;

        auto erase_from_global_list() -> void;

        auto erase_from_global_list_locked() -> void;

        gktl::Canary m_canary{this};

        /// Written once by spawn_etc()/init_first() before the thread is
        /// published, then touched only by the context switch, which runs on
        /// the CPU that owns the thread and is serialised by the runqueue
        /// lock hand-off (switch_context). No lock is taken by any reader,
        /// and none is needed; the TA_GUARDED(get_resource_lock()) it carried
        /// described a lock nobody held.
        ArchThread m_arch_thread;
        Name m_name;

        /// Null if kernel thread.
        mm::VmAspace *m_aspace;
        ustl::Arc<object::ThreadDispatcher> m_user_thread;

        /// Same story as m_arch_thread: set up before publication, afterwards
        /// only its top is read (TSS.sp0 on the switch, the AP bring-up
        /// stack). kernel_stack() hands out a reference for that.
        mm::Stack m_stack;

        /// Using a KoId if kernel thread, otherwise an user-space thread id
        /// copied from object::ThreadDispatcher.
        KoId m_tid;

        sync::SpinLock m_resource_lock;

        /// Under the PI lock. That is the lock every path that changes the
        /// thread's fate already holds -- detach(), Current::exit(),
        /// suspend_or_kill(), resume() -- and Detached is the only bit that
        /// changes after the thread is published (Idle and NoFree are set
        /// before it is). It used to say resource lock, which none of those
        /// paths took; Zircon keeps flags_ under the one thread_lock for the
        /// same reason.
        Flags m_flags TA_GUARDED(get_pi_lock());
        State m_state;
        ThreadSignals m_signals;

        sync::SpinLock m_pi_lock;

        /// Owned by the runqueue: the lock of the scheduler this thread is
        /// on (`Scheduler::get_lock()` on `Current::get(curr_cpu())`)
        /// protects it, the way Linux's sched_entity belongs to rq->lock.
        /// The scheduler reads and writes it under that lock throughout
        /// (enqueue/dequeue, evaluate_next_thread, put_prev/set_next); the
        /// PI paths that touch the profile do so while the thread is off a
        /// runqueue or with the runqueue lock also held.
        ///
        /// Not TA_GUARDED, because the analysis needs a capability
        /// *expression*, and "whichever runqueue this thread is on" is a
        /// value read out of the context itself -- there is no expression
        /// naming it from here. It said TA_GUARDED(get_pi_lock()) before,
        /// which no scheduler path satisfied, so it produced 33 warnings per
        /// translation unit and protected nothing. Linux has the same gap and
        /// closes it at run time with lockdep_assert_rq_held().
        sched::SchedEntity m_scheduling_entity;
        PreemptionState m_preemption_state;

        /// Self-synchronising: the exit side writes the retcode and then
        /// wakes m_result_wait_queue, the join side parks on that queue and
        /// reads the retcode after being woken, so the queue's own lock is
        /// what orders the two. Nothing takes the resource lock around it and
        /// the old annotation said it did. Zircon's task_state_ sits under the
        /// global thread_lock instead; this tree has no such lock, and the
        /// wait queue is the handshake.
        TaskState m_task_state;
        WaiterState m_waiter_state;
        mm::MemoryAllocationState m_memory_allocation_state;

        /// The wait queues this thread owns: the outgoing edges of its node in
        /// the profile inheritance graph.
        ///
        /// This belongs under the PI lock and not the resource lock, and the
        /// distinction is not cosmetic. Every walk of a PI chain -- thread ->
        /// owned queue -> that queue's owner -> ... -- arrives at a thread
        /// holding only its PI lock, which is the contract
        /// OwnedWaitQueue::assign_owner already asserts before it touches an
        /// owner. If the edge set hung off a second, unrelated lock, each hop
        /// would have to take two locks of different classes, and the
        /// back-off protocol that is meant to make such a walk deadlock-free
        /// orders a single lock class only. Guarding it with the resource lock
        /// also put the two directions of one edge under different locks:
        /// set_owner() ran under the PI lock while reset_owner() asserted the
        /// resource lock, so the list could be spliced and unspliced
        /// concurrently.
        ///
        /// Linux draws the line in the same place. p->pi_waiters and
        /// p->pi_blocked_on live under p->pi_lock, while alloc_lock -- what
        /// task_lock() takes -- covers what the task *holds* (->mm, ->files,
        /// ->fs) and never appears on a scheduler path.
        OwnedWaitQueue::List m_owned_wait_queues TA_GUARDED(get_pi_lock());

        /// [Optional] Available on LockDep enabled.
        [[no_unique_address]]
        lockdep::LockContext m_lock_context;

        ustl::collections::intrusive::ListMemberHook<> m_spawned_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_spawned_hook, SpawnedListOption);
        USTL_DECLARE_LIST(Thread, SpawnedList, Thread::SpawnedListOption);

        ustl::collections::intrusive::ListMemberHook<> m_migrating_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_migrating_hook, MigratingListOption);

        ustl::collections::intrusive::ListMemberHook<> m_unpark_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_unpark_hook, UnparkListOption);

        static SpawnedList SPAWNED_THREADS;
        static sync::SpinLock SPAWNED_THREADS_LOCK;

      public:
        USTL_DECLARE_LIST(Thread, MigratingList, Thread::MigratingListOption);
        USTL_DECLARE_LIST(Thread, UnparkList, Thread::UnparkListOption);
    };

    class Thread::Current: public mm::CpuLocal::Current<Thread *> {
      public:
        template <typename DurationT>
        FORCE_INLINE
        static auto sleep_for(DurationT duration, Interruptible interruptible) -> Status {
            return sleep_for(duration_cast<Duration>(duration), interruptible);
        }

        static auto sleep_for(Duration, Interruptible) -> Status;

        static auto sleep_etc(time::Deadline const &, Interruptible, TimePoint<> now) -> Status;

        static auto sleep_until(time::Deadline const &, Interruptible) -> Status;

        INIT_CODE [[noreturn]]
        static auto become_idle() -> void;

        static auto do_suspend() -> void;

        static auto process_pending_signals() -> void;

        static auto kill() -> void;

        static auto set_name(char const *name) -> void;

        [[noreturn]]
        static auto exit(i32 retcode) -> void;

        /// Preempt the current thread from an interrupt.
        ///
        /// Assumptions:
        ///     1. Interrupts has been disabled.
        static auto preempt() -> void;

        /// Schedule the current thread from non-atomic context.
        ///
        /// Assumptions:
        ///     1. Interrupts has been disabled.
        static auto reschedule() -> void;

        static auto yield() -> void;

        static auto yield_to(Self *next) -> Status;

        static auto migrate_to(CpuNum dest) -> void;

        static auto page_fault(VirtAddr va, mm::VmfCause cause) -> Status;

        static auto soft_fault(VirtAddr va, mm::VmfCause cause) -> Status;

        FORCE_INLINE
        static auto tid() -> KoId {
            return mm::CpuLocal::read(CURRENT_INSTANCE->m_tid);
        }

        FORCE_INLINE
        static auto memory_allocation_state() -> mm::MemoryAllocationState & {
            return Thread::Current::get()->memory_allocation_state();
        }

        FORCE_INLINE
        static auto waiter_state() -> WaiterState & {
            return Thread::Current::get()->waiter_state();
        }

        FORCE_INLINE
        static auto scheduling_entity() -> sched::SchedEntity & {
            return Thread::Current::get()->scheduling_entity();
        }

        FORCE_INLINE
        static auto preemption_state() -> PreemptionState & {
            return Thread::Current::get()->preemption_state();
        }

        FORCE_INLINE
        static auto aspace() -> mm::VmAspace * {
            return Thread::Current::get()->aspace();
        }

      private:
        template <typename F>
        static auto fault(VirtAddr va, F &&f) -> Status;
    };

} // namespace task
} // namespace ours

#endif // #ifndef OURS_TASK_THREAD_HPP
