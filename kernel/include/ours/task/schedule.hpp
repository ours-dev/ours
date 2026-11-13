/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHEDULE_HPP
#define OURS_TASK_SCHEDULE_HPP 1

#include <ours/compiler.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/status.hpp>
#include <ours/task/run_queue.hpp>
#include <ours/task/scheduler_config.hpp>
#include <ours/task/scheduler_profiles.hpp>
#include <ours/task/scheduler_types.hpp>

#include <ustl/dyn.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::task {
/// [[=bits::Enumask]]
enum class EnqueueOptions {
    /// Semantics.
    None,

    /// Skip the update to clock.
    SkipClock = 0_bit,

    /// Paired with DequeueOptions::Sleep. Make thread runnable.
    WakeUp = 1_bit,

    /// Paired with DequeueOptions::Migrating.
    Migrated = 2_bit,

    /// Paired with DequeueOptions::Save
    Restore = 3_bit,

    /// Paired with DequeueOptions::Simulate. Simulate a time of enqueue action
    /// without really insertion action.
    Simulate = 4_bit,

    /// Requires that the given thread is placed at front of runqueue.
    Front = 10_bit,

    Initial = 11_bit,

    /// Forcelly require the given thread to run on current CPU.
    ThisCpu = 12_bit,
};
USTL_ENABLE_ENUM_BITMASK(EnqueueOptions);

/// [[=bits::Enumask]]
enum class DequeueOptions {
    /// Semantics.
    None,

    /// Dequeue without updating scheduling clock.
    SkipClock = 0_bit,

    /// Paired with EnqueueOptions::WakeUp. Make thread no longer runnable.
    Sleep = 1_bit,

    /// Paired with EnqueueOptions::Migrated
    Migrating = 2_bit,

    /// Paired with EnqueueOptions::Restore
    Save = 3_bit,

    /// Paired with EnqueueOptions::Simulate. Simulate a time of dequeue action
    /// without really erasing action.
    Simulate = 4_bit,

    ///
    Delayed = 10_bit,
};
USTL_ENABLE_ENUM_BITMASK(DequeueOptions);

/// What the scheduler asks of a discipline, as an interface in the sense of
/// ustl::Dyn: a discipline is a plain class that has these member functions,
/// with no base class and no virtual function, and the Scheduler reaches it
/// through a ustl::Ref<Schedule>.
///
/// Five methods are required, because no discipline can do without them:
/// enqueue_thread, dequeue_thread, evaluate_next_thread, on_thread_tick and
/// num_queued. Every other method is optional. A discipline without it gets the
/// answer written beside the method below -- nothing, or a status -- once,
/// where the method is declared, instead of in every discipline.
///
/// The named accessors check the caller's lock before the erased call. In their
/// annotations |first| is the RunQueue argument. A discipline can also declare
/// `TA_REQUIRES(rq.lock())` on its implementation to check the body.

/// [Required] Insert |thread| to runqueue.
///
/// Return Status::Ok if enqueue action really happened.
USTL_DYN_MEMBER(EnqueueThread, enqueue_thread, TA_REQUIRES(first.lock()));

/// [Required] Remove |thread| from runqueue.
///
/// Return Status::Ok if dequeue action really happened. Status::Delayed if delay
/// to dequeue.
USTL_DYN_MEMBER(DequeueThread, dequeue_thread, TA_REQUIRES(first.lock()));

/// [Required] The thread that should run next on |rq|'s CPU, or null if this
/// discipline has none.
USTL_DYN_MEMBER(EvaluateNextThread, evaluate_next_thread, TA_REQUIRES(first.lock()));

/// [Required] The scheduling clock ticked while |curr| ran; |queued| says
/// whether it is on the run queue.
USTL_DYN_MEMBER(OnThreadTick, on_thread_tick, TA_REQUIRES(first.lock()));

/// [Required] Number of threads queued on |rq|'s CPU for this discipline.
///
/// The RunQueue is the parameter every other entry point takes, and it is what
/// says *which CPU* is being asked about. Without it the only implementation a
/// discipline can write is "the CPU I happen to be running on" -- which is
/// precisely the assumption that broke every cross-CPU wakeup, since
/// Scheduler::do_unpark_thread() runs on the waker and manipulates the
/// target's queues.
USTL_DYN_MEMBER(NumQueued, num_queued, TA_REQUIRES(first.lock()));

/// [Optional, does nothing] |curr| yields the CPU.
USTL_DYN_MEMBER_OR(YieldThread, yield_thread, ustl::dyn::Ignore{}, TA_REQUIRES(first.lock()));

/// [Optional, Status::NotSupported] |curr| yields to |next| in particular.
USTL_DYN_MEMBER_OR(YieldToThread, yield_to_thread, ustl::dyn::Returns<Status::NotSupported>{},
                   TA_REQUIRES(first.lock()));

/// [Optional, Status::Unimplemented] Evaluate whether preempt the thread |curr|
/// with given |thread|. Return Status::Ok if able to preempt.
USTL_DYN_MEMBER_OR(UnparkPreempt, unpark_preempt, ustl::dyn::Returns<Status::Unimplemented>{},
                   TA_REQUIRES(first.lock()));

/// [Optional, does nothing] |prev| stops being the running thread.
USTL_DYN_MEMBER_OR(PutPrevThread, put_prev_thread, ustl::dyn::Ignore{}, TA_REQUIRES(first.lock()));

/// [Optional, does nothing] |next| becomes the running thread.
USTL_DYN_MEMBER_OR(SetNextThread, set_next_thread, ustl::dyn::Ignore{}, TA_REQUIRES(first.lock()));

/// [Optional, does nothing] Update the scheduluing data of thread |curr|.
USTL_DYN_MEMBER_OR(UpdateThread, update_thread, ustl::dyn::Ignore{}, TA_REQUIRES(first.lock()));

/// [Optional, Status::Unimplemented] Replace the base profile of |thread|.
USTL_DYN_MEMBER_OR(OverrideThreadBaseProfile, override_thread_base_profile,
                   ustl::dyn::Returns<Status::Unimplemented>{}, TA_REQUIRES(first.lock()));

/// [Optional, does nothing] |thread| was created, or is the thread a CPU booted
/// on, and belongs to this discipline.
USTL_DYN_MEMBER_OR(OnThreadInit, on_thread_init, ustl::dyn::Ignore{});

/// [Optional, does nothing] |thread| is gone.
USTL_DYN_MEMBER_OR(OnThreadDead, on_thread_dead, ustl::dyn::Ignore{});

/// [Optional, does nothing] |thread| was just woken and enqueued.
USTL_DYN_MEMBER_OR(OnThreadUnpark, on_thread_unpark, ustl::dyn::Ignore{},
                   TA_REQUIRES(first.lock()));

/// [Optional, INVALID_CPU_NUM] Attempts to assign a target CPU on which |thread|
/// will run, among |candidates|. INVALID_CPU_NUM if it has no preference.
USTL_DYN_MEMBER_OR(SelectTargetCpu, select_target_cpu, ustl::dyn::Returns<INVALID_CPU_NUM>{});

/// [Optional, Status::Unsupported] Migrates |thread| onto new CPU specified by
/// |cpunum|. Return Status::Ok if the migration was performed.
USTL_DYN_MEMBER_OR(MigrateThread, migrate_thread, ustl::dyn::Returns<Status::Unsupported>{});

using Schedule = ustl::Dyn<
    ustl::Method<EnqueueThread, auto(RunQueue &, Thread *, EnqueueOptions)->Status>,
    ustl::Method<DequeueThread, auto(RunQueue &, Thread *, DequeueOptions)->Status>,
    ustl::Method<EvaluateNextThread, auto(RunQueue &, Thread *)->Thread *>,
    ustl::Method<OnThreadTick, auto(RunQueue &, Thread *, bool)->void>,
    ustl::Method<NumQueued, auto(RunQueue &)->usize>,
    ustl::Method<YieldThread, auto(RunQueue &, Thread *)->void>,
    ustl::Method<YieldToThread, auto(RunQueue &, Thread *, Thread *)->Status>,
    ustl::Method<UnparkPreempt, auto(RunQueue &, Thread *, Thread *, UnparkOptions)->Status>,
    ustl::Method<PutPrevThread, auto(RunQueue &, Thread *)->void>,
    ustl::Method<SetNextThread, auto(RunQueue &, Thread *)->void>,
    ustl::Method<UpdateThread, auto(RunQueue &, Thread *)->void>,
    ustl::Method<OverrideThreadBaseProfile,
                 auto(RunQueue &, Thread *, BaseProfile const &)->Status>,
    ustl::Method<OnThreadInit, auto(RunQueue &, Thread *)->void>,
    ustl::Method<OnThreadDead, auto(RunQueue &, Thread *)->void>,
    ustl::Method<OnThreadUnpark, auto(RunQueue &, Thread *)->void>,
    ustl::Method<SelectTargetCpu, auto(Thread *, CpuMask const &)->CpuNum>,
    ustl::Method<MigrateThread, auto(Thread *, CpuNum)->Status>>;

} // namespace ours::task

#endif // #ifndef OURS_TASK_SCHEDULE_HPP
