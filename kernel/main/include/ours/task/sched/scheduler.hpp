/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULER_HPP
#define OURS_TASK_SCHED_SCHEDULER_HPP 1

#include <ours/task/sched/types.hpp>
#include <ours/task/sched/config.hpp>
#include <ours/task/sched/runtime.hpp>

#include <ours/task/thread.hpp>

#include <ours/time/timer.hpp>

#include <ours/sync/spin-lock.hpp>
#include <ours/mem/cpu-local.hpp>

#include <ours/lockdep.hpp>

#include <ours/platform/timer.hpp>

#include <array>
#include <atomic>
#include <span>

#include <gktl/canary.hpp>

#include <ustl/views/span.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::task::sched {
    /// The core scheduler per cpu.
    class Scheduler {
        using Self = Scheduler;
      public:
        /// Scheduling mode.
        enum class Mode {
            Idle,
            Regular,
            Preempt,
        };

        /// Scheduler state
        enum class State {
            Idling,
            Active,
            Count,
        };

        class Current;

        /// First thing a freshly spawned thread must do.
        static auto trampoline_handoff() -> void;

        static auto on_thread_init(Thread *, BaseProfile const &) -> void;

        static auto on_thread_dead(Thread *) -> void;

        TA_ACQUIRE(thread->get_pi_lock())
        static auto unpark_thread(Thread *thread, UnparkControl = UnparkControl::None) -> void;

        static auto unpark_thread(Thread::UnparkList, UnparkControl = UnparkControl::None) -> void;

        // TODO(SmallHuaZi)
        static auto migrate_thread(Thread *) -> void;

        // TODO(SmallHuaZi)
        static auto set_cpu_affinity(Thread *, CpuMask const *, CpuAffinity::Kind) -> CpuMask const *;

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
            return g_states[usize(State::Active)].load(std::memoryorder_relaxed);
        }

        auto init() -> void;

        auto assert_thread_in_scheduler(Thread *thread) -> void;

        FORCE_INLINE
        auto num_runnable() const -> usize {
            return m_runtime.num_queued_threads;
        }

        FORCE_INLINE
        auto is_state_active() const -> bool {
            return g_states[usize(State::Active)].load().test(m_runtime.which_cpu);
        }

        FORCE_INLINE
        auto get_lock() -> sync::SpinLock & {
            return m_runtime.rqlock;
        }

        template <typename Op, typename Target>
        class PiOperation;
      private:
        class Migrator;

        /// [MP] 
        template <typename F, typename... Args>
        static auto find_active_scheduler_for(Thread *, F, Args &&...) -> CpuNum;

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
        TA_ACQUIRE(get_lock())
        auto reschedule_common(Thread *curr, Mode mode) -> void;

        TA_ACQUIRE(get_lock())
        auto unpark_common(Thread *curr, Mode mode) -> void;

        TA_REQUIRE(get_lock())
        auto update_runtime() -> void;

        /// Drop the current scheduler's lock, and then switch context from |curr|'s to |next|'s.
        TA_RELEASE(get_lock())
        auto switch_context(Thread *curr, Thread *next) -> void;

        auto finish_switch_context() -> void;

        TA_REQUIRE(get_lock())
        auto enqueue_thread(Thread *thread, EnqueueControl control) -> Status;

        TA_REQUIRE(get_lock())
        auto dequeue_thread(Thread *thread, DequeueControl control) -> Status;

        /// The routine called in timer interrupt routine.
        auto tick_thread(Thread *, SchedTime now) -> void;

        /// Activate the given |thread|. That means to add it onto runqueue and
        /// mark the thread ready to be scheduled.
        auto activate_thread(Thread *, EnqueueControl) -> void;

        /// Deactivate the given |thread|. That means to remove it from runqueue and
        /// disallow it to be added onto runqueue.
        auto deactivate_thread(Thread *, DequeueControl) -> void;

        /// Unpark the given |thread| with specified flags |control| on this scheduler.
        /// This method will drop the |thread|'s lock.
        ///
        /// Assumptions:
        ///     1) |thread|'s lock has been held.
        ///     2) 
        TA_ACQUIRE(get_lock())
        auto do_unpark_thread(Thread *thread, UnparkControl) -> void;

        /// Evaulates the given |thread| whether be able to preempt current thread.
        /// If it can does, send out preemption request to this scheduler.
        auto unpark_preempt(Thread *thread, UnparkControl control) -> void;

        /// Pick next most eligible thread and return it. Under multi-processor environment,
        /// it will attempts to steal a task on other remote CPUs.
        auto evaluate_next_thread(Thread *curr) -> Thread *;

        auto set_schedule(Thread *thread) -> Schedule *;

        auto get_schedule(Thread *thread) -> Schedule *;

        /// [MP] Attempts to steal work from other busy CPUs spcified by |mask| and move it 
        /// to the local run queues. Returns a pointer to the stolen thread that is now 
        /// associated with the local Scheduler instance, or nullptr is no work was stolen.
        auto steal_work_from(CpuMask const &mask) -> Thread *;

        /// [MP]
        TA_ACQUIRE(m_runtime.migrating_threads_lock)
        auto process_migrating_threads(SchedTime now) -> void;

        TA_REQUIRE(thread->get_pi_lock())
        auto finish_transition(Thread *thread) -> void;

        TA_REQUIRE(get_lock())
        auto set_state(State state, bool on) -> void;

        auto put_prev_thread(Thread *thread) -> void;

        auto set_next_thread(Thread *thread) -> void;

        gktl::Canary m_canary;
        Runtime m_runtime;
        usize m_num_switches;
        std::span<Schedule *> m_schedules;

        Thread *m_idle_thread;

        static constexpr usize kNumState = static_cast<usize>(State::Count);
        static std::array<std::atomic<CpuMask>, kNumState> g_states;
    };

    /// This helper class was designed for conviencies and securities.
    class Scheduler::Current: public mem::CpuLocal::Current<Scheduler> {
      public:
        /// Reschedule at IDLE mode
        TA_REQUIRE(curr->get_pi_lock())
        static auto idle(Thread *curr) -> void;

        /// Reschedule at Preempt mode
        TA_REQUIRE(curr->get_pi_lock())
        static auto preempt(Thread *curr) -> void;

        /// Reschedule at Regular mode
        TA_REQUIRE(curr->get_pi_lock())
        static auto reschedule(Thread *curr) -> void;

        TA_REQUIRE(curr->get_pi_lock())
        static auto yield_thread(Thread *curr) -> void;

        TA_REQUIRE(curr->get_pi_lock())
        static auto yield_to_thread(Thread *curr, Thread *next) -> Status;

        /// Called when a thread is forced to wait, e.g. waiting for a mutex or a signal.
        /// Mainly used by WaitQueue. 
        /// 
        TA_REQUIRE(curr->get_pi_lock())
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

            Guard guard{self.get_lock()};
            self.set_state(state, on);
        }
    };

} // namespace ours::task

#endif // #ifndef OURS_TASK_SCHED_SCHEDULER_HPP