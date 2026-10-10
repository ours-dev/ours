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
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/config.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/task/preemption_state.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/types.hpp>
#include <ours/task/wait_queue.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ktl/rwonce.hpp>
#include <lockdep/lock_context.hpp>
#include <sampler/stack_sample_request.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/container_of.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace logz4 {
class Domain;
} // namespace logz4

namespace ours {
namespace object {
    class ProcessDispatcher;
    class ThreadDispatcher;
    struct PendingDeletions;
} // namespace object

namespace task {
    struct ExceptionContext;
    enum class ExceptionOutcome : u8;

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

        /// Runs the entry point. It used to wake the joiners here as well, on
        /// the way back from the entry -- before the thread was even Dead, let
        /// alone off its stack. Joiners are woken through exited() now.
        FORCE_INLINE
        auto invoke() -> i32 {
            DEBUG_ASSERT(m_entry_point);
            m_retcode = m_entry_point(m_arg);
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

        /// Signalled once, with Ok, by Thread::Current::exit() of a joinable
        /// thread, once it is Dead and its return code is written -- before
        /// its final switch, while it still runs on its stack. That is safe
        /// because a joiner that wakes does not free the thread, it only drops
        /// the reaper's reference (Thread::m_refs); the thread's own reference
        /// keeps the descriptor and the stack until it is off them.
        /// Manual-reset: a join that comes after the signal returns at once.
        ///
        /// It replaces a wait queue that exit() woke under the thread's own PI
        /// lock, which took each joiner's PI lock inside it: two PI locks
        /// nested, with nothing to order them (design option B in
        /// .agents/tasks/thread-join/DESIGN.md).
        FORCE_INLINE
        auto exited() -> sync::Event & {
            return m_exited;
        }

      private:
        i32 m_retcode;
        ThreadStartRoutine m_entry_point;
        void *m_arg;
        sync::Event m_exited;
    };

    class JoinHandle;
    class UnstartedThread;

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
        /// passed to Current::exit().
        ///
        /// This call allocates a thread and places it in the global thread list,
        /// and returns it unstarted, empty if the thread could not be made. The
        /// thread will not be scheduled until UnstartedThread::start(), which
        /// is also what gives its JoinHandle.
        /// |arg| is handed to |entry| when the thread runs, and is the only
        /// context it gets. The variadic overloads that used to sit here bound
        /// their arguments into a std::function; see ThreadStartRoutine for
        /// why that could not stay. A caller with a non-static member function
        /// or an extra argument wraps it in a captureless lambda, which
        /// converts to a plain function pointer and allocates nothing.
        static auto spawn(char const *, StartRoutine, void *arg, sched::BaseProfile const &)
            -> UnstartedThread;

        static auto spawn(char const *, StartRoutine, void *arg, usize priority) -> UnstartedThread;

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
        static auto create_first(KoId, char const *, sched::BaseProfile const &, Self *) -> void;

        /// Core method family.

        /// Make the thread transition to runnable or running state.
        ///
        /// Paired with Thread::suspend. The first resume() of a thread, the one
        /// that takes it out of Initial, also takes the thread's own reference
        /// (m_refs), which a Dpc drops once it is off its stack for good.
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

        /// The same user thread, as the mutable object it is, for the object
        /// layer (object::ThreadDispatcher::current()). The accessor above
        /// cannot give it out that way: ustl::Rc makes its pointee const
        /// through a const Arc, and the dispatcher is shared, mutable state.
        /// Raw, and not an Arc, because every caller is running on this
        /// thread, which keeps the dispatcher alive for as long as it looks.
        FORCE_INLINE
        auto user_thread_mut() -> object::ThreadDispatcher * {
            return m_user_thread.as_ptr_mut();
        }

        /// The kernel objects this thread has been handed to delete while it
        /// was already deleting one, or null when it is deleting none; see
        /// object::Dispatcher::rc_recycle(). An incomplete type here -- the
        /// task layer does not know what a dispatcher is -- and borrowed, not
        /// owned: the list is a local of the outermost deletion on this
        /// thread, registered for that call's length by a
        /// PendingDeletionsScope. Only ever touched by the thread itself, so
        /// it needs no lock.
        FORCE_INLINE
        auto pending_deletions() const -> object::PendingDeletions * {
            return m_pending_deletions;
        }

        /// Registers |deletions| as this thread's pending deletions for the
        /// scope's length, and withdraws them as the scope ends, on whichever
        /// path it ends by. The list is a local of the call that opens the
        /// scope: a pointer to it left behind would send the next deletion on
        /// this thread into a dead stack frame.
        class PendingDeletionsScope final {
          public:
            [[nodiscard]]
            PendingDeletionsScope(Thread &thread, object::PendingDeletions &deletions)
                : m_thread(thread) {
                m_thread.m_pending_deletions = &deletions;
            }

            USTL_NO_MOVEABLE_AND_COPYABLE(PendingDeletionsScope);

            ~PendingDeletionsScope() {
                m_thread.m_pending_deletions = nullptr;
            }

          private:
            Thread &m_thread;
        };

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
            // read_once: a thread reads its own signals without the PI lock
            // (process_pending_signals()) while another CPU may be setting
            // them under it.
            return ktl::read_once(m_signals);
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
        /// ceremony. No bit of m_flags moves after publication now that
        /// Detached and Joined are gone.
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

        /// The slot logz4 keeps this thread's current log domain in; see
        /// m_log_domain.
        FORCE_INLINE
        auto log_domain() -> logz4::Domain const *& {
            return m_log_domain;
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
        /// Bits 2 and 5 were Detached and Joined, which decided which one
        /// party freed a thread. The JoinHandle and m_refs replaced them: no
        /// party frees a thread, the last reference out does.
        enum class Flags : u32 {
            Interruptible = 0_bit,
            SignalPending = 1_bit,

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
        friend JoinHandle;
        friend UnstartedThread;

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

        /// Drops one of the references in m_refs, and destroys the thread when
        /// it was the last. The last drop may free the thread there and then,
        /// so it must come where blocking is allowed: interrupts and
        /// preemption on.
        auto drop_ref() -> void;

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

        /// See pending_deletions().
        object::PendingDeletions *m_pending_deletions = nullptr;

        /// Same story as m_arch_thread: set up before publication, afterwards
        /// only its top is read (TSS.sp0 on the switch, the AP bring-up
        /// stack). kernel_stack() hands out a reference for that.
        mm::Stack m_stack;

        /// Using a KoId if kernel thread, otherwise an user-space thread id
        /// copied from object::ThreadDispatcher.
        KoId m_tid;

        /// Guards what the thread *holds* -- such as its memory allocation
        /// policy, which may be replaced only under this lock -- as
        /// opposed to its place in the PI graph, which is the PI lock's (see
        /// m_owned_wait_queues). It is taken IrqSave: that is the rule for
        /// every spin lock in the tree, and one of its holders --
        /// PmNode::Current::apply_thread_policy(), reading the memory policy --
        /// runs on the frame-allocation path, which the page fault path
        /// reaches with interrupts already off.
        sync::SpinLock m_resource_lock;

        /// Under the PI lock. That is the lock every path that changes the
        /// thread's fate already holds -- Current::exit(), suspend_or_kill(),
        /// resume() -- though no bit changes after the thread is published
        /// any more (Idle and NoFree are set before it is; Detached, the one
        /// that moved, went with the JoinHandle). It used to say resource
        /// lock, which none of those paths took.
        Flags m_flags TA_GUARDED(get_pi_lock());

        /// Who still needs the descriptor; it is freed by whoever drops the
        /// last reference (drop_ref()). At most two:
        ///  - the JoinHandle's, from spawn(), dropped when the handle is
        ///    dropped or joins;
        ///  - the thread's own, taken by its first resume() and dropped by a
        ///    Dpc that runs after its final switch: a CPU may run on its stack
        ///    until then, and the next thread holds its PI lock.
        /// Neither side knows whether the other has gone, and need not: that
        /// is the point. A thread never resumed has only the handle's, so
        /// dropping the handle frees it unrun. A thread that never exits (an
        /// idle thread, the boot thread) never drops its own.
        std::atomic<u32> m_refs{1};
        State m_state;
        ThreadSignals m_signals;

        /// The sampler's request to walk this thread's user stack on its way
        /// back to user space, kept here, opaque, while ThreadSignals::
        /// SampleStack is raised (request_stack_sample()).
        sampler::StackSampleRequest m_stack_sample;

        /// Held with interrupts off by every holder: the tick's
        /// Thread::Current::preempt() takes the running thread's from interrupt
        /// context, and a sleep's timeout takes the sleeper's from its timer
        /// callback. A holder whose scope reaches reschedule_common() or wakes
        /// a thread holds preemption off as well -- IrqSaveAndPreemptDisable --
        /// so that a reschedule the wake asks for waits until the lock is
        /// dropped.
        sync::SpinLock m_pi_lock;

        /// Owned by the runqueue: the lock of the scheduler this thread is
        /// on (`Scheduler::get_lock()` on `Current::get(curr_cpu())`)
        /// protects it. The scheduler reads and writes it under that lock
        /// throughout (enqueue/dequeue, evaluate_next_thread,
        /// put_prev/set_next); the PI paths that touch the profile do so while
        /// the thread is off a runqueue or with the runqueue lock also held.
        ///
        /// Not TA_GUARDED, because the analysis needs a capability
        /// *expression*, and "whichever runqueue this thread is on" is a
        /// value read out of the context itself -- there is no expression
        /// naming it from here. It said TA_GUARDED(get_pi_lock()) before,
        /// which no scheduler path satisfied, so it produced 33 warnings per
        /// translation unit and protected nothing. The gap can be closed only
        /// at run time, by asserting that the runqueue's lock is held.
        sched::SchedEntity m_scheduling_entity;
        PreemptionState m_preemption_state;

        /// Self-synchronising: the exit side writes the retcode under the PI
        /// lock and later signals TaskState::exited(), the join side waits on
        /// that event and reads the retcode after it returns, so the event's
        /// own lock is what orders the two. Nothing takes the resource lock
        /// around it and the old annotation said it did. There is no global
        /// thread lock to put it under: the event is the handshake.
        TaskState m_task_state;
        WaiterState m_waiter_state;
        mm::MemoryAllocationState m_memory_allocation_state;

        /// The logz4::LogDomain scope this thread is in, or null: what every
        /// line it logs through the free functions is attributed to.
        ///
        /// Per thread and not per CPU, because a thread can sleep inside a
        /// scope -- kshell does, waiting on the serial port -- and whatever
        /// runs on its CPU meanwhile has not entered it. Unguarded: the
        /// thread itself is the only writer (the guard runs on it) and the
        /// only reader (so does vlog()). An interrupt that arrives while the
        /// thread is in a scope neither reads nor writes this; it gets a slot
        /// of its own (kernel/lib/logz4/kernel.cpp).
        logz4::Domain const *m_log_domain = nullptr;

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
        /// That is where the line between the two locks falls: the PI lock
        /// covers the thread's edges in the inheritance graph, which every PI
        /// walk reaches, while the resource lock covers what the thread
        /// *holds*, such as its memory allocation policy, and no PI walk takes
        /// it.
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

        /// Always taken with interrupts off (`Guard<SpinLock, IrqSave>`),
        /// because Thread::Current::exit() takes it with interrupts off.
        ///
        /// Every dying thread unlinks itself from inside exit()'s
        /// interrupts-off, preemption-off section. If spawn_etc() on the same
        /// CPU held this lock
        /// with interrupts on, a tick could preempt it mid-section; the exiting
        /// thread would then spin on a lock whose holder sits in this CPU's run
        /// queue (Scheduler::steal_work_from() is a stub, so no other CPU takes
        /// it), and this CPU never reschedules while it spins with interrupts
        /// off: a hang in dbg_0, SpinLock::acquire()'s !is_held() assertion in
        /// dbg_1. Once any holder runs with interrupts off, all of them must.
        static sync::SpinLock SPAWNED_THREADS_LOCK;

      public:
        USTL_DECLARE_LIST(Thread, MigratingList, Thread::MigratingListOption);
        USTL_DECLARE_LIST(Thread, UnparkList, Thread::UnparkListOption);
    };

    /// The one owner of a thread that Thread::spawn() made, the way its
    /// creator starts, joins or gives it up. It holds one of the thread's
    /// references (Thread::m_refs); the thread is freed when that one and the
    /// thread's own are both gone, whichever goes second.
    ///
    /// Move-only: a thread has one handle, so one joiner. Dropping a handle
    /// that has not joined detaches the thread: it runs on and is freed once
    /// it has exited and left its stack. Since a drop may free the thread
    /// there and then, a handle is dropped only where blocking is allowed
    /// (Thread::drop_ref()).
    ///
    /// A handle comes from UnstartedThread::start(), so the thread behind it
    /// has been started. The one exception is UnstartedThread::defer_start(),
    /// whose handle holds a thread its first resume() starts; dropped before
    /// that, it frees the thread unrun.
    class JoinHandle {
        using Self = JoinHandle;

      public:
        JoinHandle() = default;
        JoinHandle(Self &&other) noexcept;
        auto operator=(Self &&other) noexcept -> Self &;
        JoinHandle(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;
        ~JoinHandle();

        FORCE_INLINE
        explicit operator bool() const {
            return m_thread != nullptr;
        }

        /// The thread, for as long as this handle owns it. Not to be kept
        /// past the handle: once the handle is gone, so may the thread be.
        FORCE_INLINE
        auto get() const -> Thread * {
            return m_thread;
        }

        FORCE_INLINE
        auto operator->() const -> Thread * {
            DEBUG_ASSERT(m_thread, "an empty JoinHandle used");
            return m_thread;
        }

        /// Waits until the thread has exited -- it is Dead and its return code
        /// is written, though it may still be on its stack -- then stores the
        /// return code in |retcode| when that is not null and gives the thread
        /// up, leaving this handle empty. Not interruptible; ends at
        /// |deadline|, and on TimeOut nothing changes: the handle still owns
        /// the thread and may join again or be dropped. The caller must not be
        /// the thread itself.
        ///
        /// The wait is on TaskState::exited(), which the exiting thread
        /// signals before its final switch; the thread's own reference is what
        /// makes giving it up then safe. `threadjoin.test` checks this
        /// contract.
        ///
        /// What a return does not say: that the thread is off its CPU. It may
        /// still be running its last instructions on its stack, in its address
        /// space, holding its PI lock. So a caller must not tear down anything
        /// the thread's final switch still uses -- the address space it ran
        /// in, above all, whose freeing needs no CPU to have it loaded
        /// (ArchVmAspace::destroy()) -- on the strength of a join alone. A
        /// thread that ran in an address space of the caller's switches off it
        /// itself before it returns, as ioport.test's observer does. The
        /// thread is off its CPU only once its own reference is gone, which
        /// nothing outside it observes today.
        auto join(i32 *retcode, time::Deadline deadline) -> Status;

        /// Gives the thread up without waiting for it, leaving this handle
        /// empty: what dropping the handle does, said where it happens. Does
        /// nothing to an empty handle.
        auto detach() -> void;

      private:
        friend UnstartedThread;

        FORCE_INLINE
        explicit JoinHandle(Thread *thread)
            : m_thread(thread) {}

        Thread *m_thread = nullptr;
    };

    /// A thread Thread::spawn() has made and nobody has started: the state in
    /// which it is configured -- its affinity, its address space -- before it
    /// can run. start() is the way on to its JoinHandle, so a thread cannot
    /// be joined before it was started, which would wait for good, nor given
    /// up before it was started, which would free it unrun.
    ///
    /// Move-only, and ended by one of start(), defer_start() and cancel(),
    /// each of which consumes it (`ustl::move(thread).start()`). One dropped
    /// with none of them is a bug -- a thread meant to run that never will
    /// -- and a debug build stops on it; it is freed unrun either way.
    class [[nodiscard]] UnstartedThread {
        using Self = UnstartedThread;

      public:
        UnstartedThread() = default;
        UnstartedThread(Self &&other) noexcept;
        auto operator=(Self &&other) noexcept -> Self &;
        UnstartedThread(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;
        ~UnstartedThread();

        FORCE_INLINE
        explicit operator bool() const {
            return m_thread != nullptr;
        }

        /// The thread, to configure before it starts.
        FORCE_INLINE
        auto operator->() const -> Thread * {
            DEBUG_ASSERT(m_thread, "an empty UnstartedThread used");
            return m_thread;
        }

        /// Starts the thread and hands it to its JoinHandle. An empty one --
        /// from a spawn that failed -- gives an empty handle.
        [[nodiscard]]
        auto start() && -> JoinHandle;

        /// Hands the thread to its JoinHandle without starting it, leaving
        /// the start to the handle's owner: the first resume() through the
        /// handle is what starts it. For an owner that holds a thread before
        /// it may run and starts it along its own path -- ThreadDispatcher,
        /// from a thread's start to its activation. Dropping the handle before
        /// that resume() frees the thread unrun, which is then what was meant.
        [[nodiscard]]
        auto defer_start() && -> JoinHandle;

        /// Frees the thread unrun: for a caller that spawned a thread and
        /// then could not go on to start it.
        auto cancel() && -> void;

      private:
        friend class Thread;

        FORCE_INLINE
        explicit UnstartedThread(Thread *thread)
            : m_thread(thread) {}

        Thread *m_thread = nullptr;
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

        /// The sampling timer interrupted the running thread in user space
        /// and asks for its user stack. Keeps |request| and raises
        /// SampleStack, which process_pending_signals() acts on before the
        /// thread returns to user space, by carrying the request out: the
        /// user stack can be read there, with interrupts on, but not in the
        /// interrupt. Interrupt context, on the thread's own CPU.
        static auto request_stack_sample(sampler::StackSampleRequest request) -> void;

        /// The running user thread raised |context| in ring 3 and the
        /// architecture cannot resolve it. Hands it to the thread's
        /// ThreadDispatcher and passes on its answer: Resume, or End, in
        /// which case the thread exits when the caller next processes its
        /// signals. Interrupts must be on: the object layer takes sleeping
        /// locks.
        [[nodiscard]]
        static auto on_user_exception(ExceptionContext const &context) -> ExceptionOutcome;

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

        /// Through get(), like every accessor below. CpuLocal::read() takes a
        /// CPU_LOCAL variable itself, never a field reached through one:
        /// `CURRENT_INSTANCE->m_tid` dereferences the template copy, which is
        /// a null pointer, so the "variable" read() was given was address
        /// 0x88, and %gs:0x88 is no address at all.
        FORCE_INLINE
        static auto tid() -> KoId {
            return Thread::Current::get()->tid();
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
