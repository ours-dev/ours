/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_THREAD_HPP
#define OURS_TASK_THREAD_HPP 1

#if !__has_include(<ours/arch/thread.hpp>)
#   error "The header <ours/arch/thread.hpp> is required by [protocol.module.task]"
#endif
#include <ours/arch/thread.hpp>

#include <ours/signals.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/compiler.hpp>

#include <ours/task/types.hpp>
#include <ours/task/config.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/preemption-state.hpp>

#include <ours/task/wait-queue.hpp>
#include <ours/task/owned-wait-queue.hpp>
#include <ours/sync/spin-lock.hpp>

#include <ours/mm/types.hpp>
#include <ours/mm/stack.hpp>
#include <ours/mm/cpu-local.hpp>
#include <ours/mm/memory-allocation-state.hpp>

#include <ours/syscall/time.hpp>

#include <ustl/util/enum_bits.hpp>

#include <ustl/rc.hpp>
#include <ustl/mem/container_of.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/collections/intrusive/list.hpp>

#include <ktl/name.hpp>
#include <ktl/rwonce.hpp>
#include <gktl/canary.hpp>

#include <lockdep/lock-context.hpp>

namespace ours {
namespace object {
    class ProcessDispatcher;
    class ThreadDispatcher;
} // namespace object

namespace task {
    using sched::LOWEST_PRIORITY;
    using sched::HIGHEST_PRIORITY;
    using sched::DEFAULT_PRIORITY;

    class TaskState {
        using Self = TaskState;
      public:
        FORCE_INLINE
        auto init(ThreadStartRoutine entry) -> void {
            m_entry_point = entry; 
            m_retcode = 0;
        }

        FORCE_INLINE
        auto invoke() -> i32 {
            DEBUG_ASSERT(m_entry_point);
            m_retcode = m_entry_point();
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
        sync::WaitQueue m_result_wait_queue;
    };

    class Thread {
        using Self = Thread;
      public:
        enum class State: u16 {
           /// The unique states
           Running = 0_bit,
           Ready   = 1_bit,
           Dead    = 2_bit,

           Suspend = 3_bit,
           Initial = 4_bit,

           Parked  = 5_bit,
           Waking  = 6_bit,
           Sleeping = 7_bit,

           Special = Parked | Sleeping | Suspend,
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(State);

        friend auto to_string(Thread::State state) -> char const *;

        using Name = ktl::Name<32>;
        using TrampolineRoutine = auto (*)() -> void;
        using StartRoutine = ustl::function::Fn<auto () -> i32>;

        class Current;

        /// Creates a thread with `name` that will execute `entry` at `priority`. |arg|
        /// will be passed to `entry` when executed, the return value of `entry` will be
        /// passed to Exit().
        ///
        /// This call allocates a thread and places it in the global thread list. This
        /// memory will be freed by either Join() or Detach(), one of these must be called.
        /// The thread will not be scheduled until resume() is called.
        static auto spawn(char const *, StartRoutine, sched::BaseProfile const &) -> Self *;

        static auto spawn(char const *, StartRoutine, usize priority) -> Self *;

        template <typename F, typename... Args>
        static auto spawn(char const *, sched::BaseProfile const &, F entry, Args&&...) -> Self *;

        template <typename F, typename... Args>
        static auto spawn(char const *, usize priority, F entry, Args&&...) -> Self *;

        static auto spawn_etc(KoId, char const *, StartRoutine, sched::BaseProfile const &, 
                              TrampolineRoutine, Self *) -> Status;

        /// Core method family.

        auto join(i32 *retcode, time::Deadline deadline) -> Status;

        auto detach() -> Status;

        /// Make the thread transition to runnable or running state. 
        ///
        /// Paired with Thread::suspend.
        auto resume() -> Status;

        auto detach_and_resume() -> Status;

        auto suspend() -> Status { return suspend_or_kill(SuspendOrKillOp::Suspend); }

        auto kill() -> Status { return suspend_or_kill(SuspendOrKillOp::Kill); }

        auto set_cpu_affinity_hard(CpuMask const &) -> void;

        auto set_cpu_affinity_soft(CpuMask const &) -> void;

        auto override_base_profile(sched::BaseProfile const &profile) -> void;

        auto bind_user_thread(ustl::Arc<object::ThreadDispatcher> user_thread) -> void;

        /// The followings is a group of accessor.
        FORCE_INLINE
        auto tid() -> KoId { return m_tid; }

        FORCE_INLINE
        auto state() -> State { return m_state; }

        FORCE_INLINE
        auto signals() -> Signals { return m_signals; }

        FORCE_INLINE
        auto aspace() -> mm::VmAspace * { return m_aspace; }

        FORCE_INLINE
        auto kernel_stack() -> mm::Stack & { return m_stack; }

        FORCE_INLINE
        auto last_cpu() const -> CpuNum { return m_scheduling_context.last_cpu(); }

        FORCE_INLINE
        auto curr_cpu() const -> CpuNum { return m_scheduling_context.curr_cpu(); }

        FORCE_INLINE
        auto is_state_dead() const -> bool { return m_state == State::Dead; }

        FORCE_INLINE
        auto is_state_initial() const -> bool { return m_state == State::Initial; }

        FORCE_INLINE
        auto is_state_ready() const -> bool { return m_state == State::Ready; }

        FORCE_INLINE
        auto is_state_sleeping() const -> bool { return m_state == State::Sleeping; }

        FORCE_INLINE
        auto is_state_parked() const -> bool { return m_state == State::Parked; }

        FORCE_INLINE
        auto is_state_running() const -> bool { return m_state == State::Running; }

        FORCE_INLINE
        auto is_state_suspend() const -> bool { return m_state == State::Suspend; }

        FORCE_INLINE
        auto is_state_special() const -> bool { return m_state == State::Special; }

        FORCE_INLINE
        auto is_idle_thread() const -> bool { return !!(m_flags & Flags::Idle); }

        FORCE_INLINE
        auto name() const -> char const * { return m_name.data(); }

        FORCE_INLINE
        auto get_resource_lock() -> sync::SpinLock & { return m_resource_lock; }

        FORCE_INLINE
        auto get_pi_lock() -> sync::SpinLock & { return m_pi_lock; }

        FORCE_INLINE
        auto waiter_state() -> sync::WaiterState & { return m_waiter_state; }

        FORCE_INLINE
        auto preemption_state() -> PreemptionState & { return m_preemption_state; }

        FORCE_INLINE
        auto memory_allocation_state() -> mm::MemoryAllocationState & { return m_memory_allocation_state; }

        FORCE_INLINE
        auto check_kill_signal() const -> bool { return !!(m_signals & Signals::Kill); }

        FORCE_INLINE
        auto check_kill_or_suspend_signal() const -> Status { 
            if (!!(m_signals & Signals::Kill)) {
                return Status::InternalIrqKilled;
            } else if (!!(m_signals & Signals::Suspend)) {
                return Status::InternalIrqRetry;
            }

            return Status::Ok;
        }
      private:
        enum class Flags: u32 {
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
        static auto of(sched::Context *se) -> Self * {
            return ustl::mem::container_of(se, &Self::m_scheduling_context);
        }

        FORCE_INLINE
        static auto of(ArchThread *arch) -> Self * {
            return ustl::mem::container_of(arch, &Self::m_arch_thread);
        }

        FORCE_INLINE
        static auto of(sync::WaiterState *waiter) -> Self * {
            return ustl::mem::container_of(waiter, &Self::m_waiter_state);
        }

        Thread(KoId tid, char const *name, sched::BaseProfile const &profile);

        ~Thread() = default;

        FORCE_INLINE
        auto scheduling_context() -> sched::Context & { 
            return m_scheduling_context;
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

        FORCE_INLINE
        auto set_flag_idle() -> void {
            m_flags |= Flags::Idle;
        }

        FORCE_INLINE
        auto set_flag_nofree() -> void {
            m_flags |= Flags::NoFree;
        }

        friend ArchThread;
        friend IdleThread;

        friend sched::Scheduler;
        friend sched::Fair;
        friend class object::ProcessDispatcher;

        friend sync::WaitQueue;
        friend sync::OwnedWaitQueue;

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

        gktl::Canary m_canary;
        ArchThread m_arch_thread TA_GUARD(get_resource_lock());
        Name m_name;

        /// Null if kernel thread.
        mm::VmAspace *m_aspace;
        ustl::Arc<object::ThreadDispatcher> m_user_thread;
        mm::Stack m_stack TA_GUARD(get_resource_lock());

        /// Using a KoId if kernel thread, otherwise an user-space thread id
        /// copied from object::ThreadDispatcher.
        KoId m_tid;

        sync::SpinLock m_resource_lock;
        Flags m_flags TA_GUARD(get_resource_lock());
        State m_state;
        Signals m_signals;

        sync::SpinLock m_pi_lock;
        sched::Context m_scheduling_context TA_GUARD(get_pi_lock());
        PreemptionState m_preemption_state;
        TaskState m_task_state TA_GUARD(get_resource_lock());
        sync::WaiterState m_waiter_state;
        mm::MemoryAllocationState m_memory_allocation_state;
        sync::OwnedWaitQueue::List m_owned_wait_queues;

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

        static SpawnedList g_spawned_threads;
        static sync::SpinLock g_spawned_threads_lock;
      public:
        USTL_DECLARE_LIST(Thread, MigratingList, Thread::MigratingListOption);
        USTL_DECLARE_LIST(Thread, UnparkList, Thread::UnparkListOption);
    };

    template <typename F, typename... Args>
    FORCE_INLINE
    auto Thread::spawn(const char *name, usize priority, F entry, Args&&... args) -> Self * {
        StartRoutine entry_point = ustl::function::bind(entry, ustl::forward<Args>(args)...);
        return Self::spawn(name, priority, entry_point);
    }

    template <typename F, typename... Args>
    FORCE_INLINE
    auto Thread::spawn(const char *name, sched::BaseProfile const &profile, F entry, Args&&... args) -> Self * {
        StartRoutine entry_point = ustl::function::bind(entry, ustl::forward<Args>(args)...);
        return Self::spawn(name, profile, entry_point);
    }

    class Thread::Current: public mm::CpuLocal::Current<Thread *> {
      public:
        template <typename DurationT>
        FORCE_INLINE
        static auto sleep_for(DurationT duration, sync::Interruptible interruptible) -> Status {
            return sleep_for(duration_cast<Duration>(duration), interruptible);
        }

        static auto sleep_for(Duration, sync::Interruptible) -> Status;

        static auto sleep_etc(time::Deadline const &, sync::Interruptible, TimePoint<> now) -> Status;

        static auto sleep_until(time::Deadline const &, sync::Interruptible) -> Status;

        [[noreturn]] INIT_CODE
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
            return mm::CpuLocal::read(g_current_instance->m_tid);
        }

        FORCE_INLINE
        static auto memory_allocation_state() -> mm::MemoryAllocationState & {
            return Thread::Current::get()->memory_allocation_state();
        }

        FORCE_INLINE
        static auto waiter_state() -> sync::WaiterState & {
            return Thread::Current::get()->waiter_state();
        }

        FORCE_INLINE
        static auto scheduling_context() -> sched::Context & {
            return Thread::Current::get()->scheduling_context();
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

} // namespace ours::task
} // namespace ours

#endif // #ifndef OURS_TASK_THREAD_HPP
