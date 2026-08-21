/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULE_HPP
#define OURS_TASK_SCHED_SCHEDULE_HPP 1

#include <ours/task/sched/types.hpp>
#include <ours/task/sched/profiles.hpp>
#include <ours/task/sched/config.hpp>
#include <ours/task/sched/runtime.hpp>

#include <ours/status.hpp>
#include <ours/compiler.hpp>
#include <ours/cpu-mask.hpp>

#include <ustl/util/enum_bits.hpp>

#include <gktl/canary.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::task::sched {
    /// [[=bits::Enumask]]
    enum class EnqueueControl {
        /// Semantics.
        None,

        /// Skip the update to clock.
        SkipClock = 0_bit,

        /// Paired with DequeueControl::Sleep. Make thread runnable.
        WakeUp = 1_bit,

        /// Paired with DequeueControl::Migrating.
        Migrated = 2_bit,

        /// Paired with DequeueControl::Save
        Restore = 3_bit,

        /// Paired with DequeueControl::Simulate. Simulate a time of enqueue action
        /// without really insertion action. 
        Simulate = 4_bit,

        /// Requires that the given thread is placed at front of runqueue.
        Front = 10_bit,

        Initial = 11_bit,

        /// Forcelly require the given thread to run on current CPU.
        ThisCpu = 12_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(EnqueueControl);

    /// [[=bits::Enumask]]
    enum class DequeueControl {
        /// Semantics.
        None,

        /// Dequeue without updating scheduling clock.
        SkipClock = 0_bit,

        /// Paired with EnqueueControl::WakeUp. Make thread no longer runnable.
        Sleep = 1_bit,

        /// Paired with EnqueueControl::Migrated
        Migrating = 2_bit,

        /// Paired with EnqueueControl::Restore
        Save = 3_bit,

        /// Paired with EnqueueControl::Simulate. Simulate a time of dequeue action
        /// without really erasing action. 
        Simulate = 4_bit,

        /// 
        Delayed = 10_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(DequeueControl);

    /// Schedule operation should be exclusively implemented for per discipline.
    class ISchedule {
        using Self = ISchedule;
      public:
        /// [Required] Insert |thread| to runqueue.
        ///
        /// Requires the runqueue lock of runtime is been holding by caller.
        ///
        /// Return Status::Ok if enqueue action really happened.
        TA_REQUIRES(runtime.rqlock)
        virtual auto enqueue_thread(Runtime &runtime, Thread *, EnqueueControl) -> Status = 0;

        /// [Required] Remove |thread| from runqueue.
        ///
        /// Return Status::Ok if dequeue action really happened. Status::Delayed if delay
        /// to dequeue.
        TA_REQUIRES(runtime.rqlock)
        virtual auto dequeue_thread(Runtime &runtime, Thread *, DequeueControl) -> Status = 0;

        virtual auto yield_thread(Runtime &, Thread *curr) -> void {}
        virtual auto yield_to_thread(Runtime &, Thread *curr, Thread *next) -> Status {
            return Status::NotSupported;
        }

        /// Evaluate whether preempt the thread |curr| with given |thread| if needed.
        ///
        /// Return Status::Ok if able to preempt.
        virtual auto unpark_preempt(Runtime &, Thread *curr, Thread *thread, UnparkControl) -> Status {
            return Status::Unimplemented;
        }

        virtual auto evaluate_next_thread(Runtime &, Thread *curr) -> Thread * = 0;
        virtual auto put_prev_thread(Runtime &, Thread *prev) -> void {}
        virtual auto set_next_thread(Runtime &, Thread *next) -> void {}

        /// [Optional] Update the scheduluing data of thread |curr|.
        virtual auto update_thread(Runtime &, Thread *curr) -> void {}

        virtual auto override_thread_base_profile(Runtime &, Thread *, BaseProfile const &) -> Status {
            return Status::Unimplemented;
        }

        virtual auto on_thread_init(Runtime &, Thread *) -> void {}
        virtual auto on_thread_dead(Runtime &, Thread *) -> void {}
        virtual auto on_thread_unpark(Runtime &, Thread *) -> void {}

        virtual auto on_thread_tick(Runtime &, Thread *, bool queued) -> void = 0;

        /// [Optional] Attempts to assign a target CPU on which |thread| will run.
        ///
        /// Return `kInvalidCpuNum` if failed to assign.
        virtual auto select_target_cpu(Thread *, CpuMask const &) -> CpuNum {
            return INVALID_CPU_NUM;
        }

        /// [Optional] Migrates |thread| onto new CPU specified by |cpunum|.
        ///
        /// Return `kInvalidCpuNum` if successfull.
        virtual auto migrate_thread(Thread *, CpuNum) -> Status {
            return Status::Unsupported;
        }
      protected:
        USTL_NO_MOVEABLE_AND_COPYABLE(ISchedule);

        virtual ~ISchedule() = default;
    };

    template <typename Derived, Discipline Discipline>
    class Schedule: public ISchedule {
       
      private:
        static Derived X; 
    };

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULE_HPP
