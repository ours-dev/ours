/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHEDULE_HPP
#define OURS_TASK_SCHED_SCHEDULE_HPP 1

#include <ours/compiler.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/status.hpp>
#include <ours/task/sched/config.hpp>
#include <ours/task/sched/profiles.hpp>
#include <ours/task/sched/runtime.hpp>
#include <ours/task/sched/types.hpp>

#include <gktl/canary.hpp>
#include <gktl/types.hpp>
#include <ustl/util/enum_bits.hpp>
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
///
/// Every parameter here is named, and on the |Runtime &| that is a
/// requirement rather than a courtesy: a clang thread-safety attribute names
/// an expression, and there is no expression for an unnamed parameter -- so
/// `TA_REQUIRES(runtime.rqlock)` cannot even be written on a hook declared
/// `(Runtime &, Thread *curr)`. Two of the eighteen entry points below say
/// which lock they are called under; the rest are called under the same lock
/// and said nothing, and the reason they said nothing is that they could not.
///
/// This is why Linux's sched_class hooks name theirs too
/// (`enqueue_task(struct rq *rq, struct task_struct *p, int flags)`): the
/// declaration is where the contract is read.
class ISchedule {
    using Self = ISchedule;

  public:
    /// [Required] Insert |thread| to runqueue.
    ///
    /// Requires the runqueue lock of runtime is been holding by caller.
    ///
    /// Return Status::Ok if enqueue action really happened.
    TA_REQUIRES(runtime.rqlock)
    virtual auto enqueue_thread(Runtime &runtime, Thread *thread, EnqueueControl control)
        -> Status = 0;

    /// [Required] Remove |thread| from runqueue.
    ///
    /// Return Status::Ok if dequeue action really happened. Status::Delayed if delay
    /// to dequeue.
    TA_REQUIRES(runtime.rqlock)
    virtual auto dequeue_thread(Runtime &runtime, Thread *thread, DequeueControl control)
        -> Status = 0;

    TA_REQUIRES(runtime.rqlock)
    virtual auto yield_thread(Runtime &runtime, Thread *curr) -> void {}
    TA_REQUIRES(runtime.rqlock)
    virtual auto yield_to_thread(Runtime &runtime, Thread *curr, Thread *next) -> Status {
        return Status::NotSupported;
    }

    /// Evaluate whether preempt the thread |curr| with given |thread| if needed.
    ///
    /// Return Status::Ok if able to preempt.
    TA_REQUIRES(runtime.rqlock)
    virtual auto unpark_preempt(Runtime &runtime, Thread *curr, Thread *thread,
                                UnparkControl control) -> Status {
        return Status::Unimplemented;
    }

    TA_REQUIRES(runtime.rqlock)
    virtual auto evaluate_next_thread(Runtime &runtime, Thread *curr) -> Thread * = 0;
    TA_REQUIRES(runtime.rqlock)
    virtual auto put_prev_thread(Runtime &runtime, Thread *prev) -> void {}
    TA_REQUIRES(runtime.rqlock)
    virtual auto set_next_thread(Runtime &runtime, Thread *next) -> void {}

    /// [Optional] Update the scheduluing data of thread |curr|.
    TA_REQUIRES(runtime.rqlock)
    virtual auto update_thread(Runtime &runtime, Thread *curr) -> void {}

    virtual auto override_thread_base_profile(Runtime &runtime, Thread *thread,
                                              BaseProfile const &profile) -> Status {
        return Status::Unimplemented;
    }

    virtual auto on_thread_init(Runtime &runtime, Thread *thread) -> void {}
    virtual auto on_thread_dead(Runtime &runtime, Thread *thread) -> void {}
    TA_REQUIRES(runtime.rqlock)
    virtual auto on_thread_unpark(Runtime &runtime, Thread *thread) -> void {}

    virtual auto on_thread_tick(Runtime &runtime, Thread *curr, bool queued) -> void = 0;

    /// Number of threads queued on |runtime|'s CPU for this schedule.
    ///
    /// The Runtime is the parameter every other entry point here takes, and it
    /// is what says *which CPU* is being asked about. Without it the only
    /// implementation an ISchedule can write is "the CPU I happen to be
    /// running on" -- which is precisely the assumption that broke every
    /// cross-CPU wakeup, since Scheduler::do_unpark_thread() runs on the waker
    /// and manipulates the target's queues.
    [[nodiscard]]
    TA_REQUIRES(runtime.rqlock) virtual auto num_queued(Runtime &runtime) const -> usize = 0;

    /// [Optional] Attempts to assign a target CPU on which |thread| will run.
    ///
    /// Return `INVALID_CPU_NUM` if failed to assign.
    virtual auto select_target_cpu(Thread *thread, CpuMask const &candidates) -> CpuNum {
        return INVALID_CPU_NUM;
    }

    /// [Optional] Migrates |thread| onto new CPU specified by |cpunum|.
    ///
    /// Return `Status::Ok` if the migration was performed.
    virtual auto migrate_thread(Thread *thread, CpuNum cpunum) -> Status {
        return Status::Unsupported;
    }

    /// Which discipline this schedule implements. Implemented once, by
    /// `Schedule<Derived, Discipline>` below, from its own template
    /// argument -- every concrete discipline gets it for free.
    [[nodiscard]]
    virtual auto discipline() const -> sched::Discipline = 0;

  protected:
    USTL_NO_MOVEABLE_AND_COPYABLE(ISchedule);

    /// Declaring the deleted copy/move members above suppresses the
    /// implicit default constructor; disciplines are default-constructed
    /// (see Schedule<>::INSTANCE), so bring it back explicitly.
    ISchedule() = default;

    virtual ~ISchedule() = default;
};

/// One entry per registered discipline, collected by the linker into a
/// single section (see `Schedule::REGISTRATION` below and the
/// __schedules_start/__schedules_end block in //kernel/kernel.ld).
/// `instance` is the discipline's one
/// shared, stateless ISchedule object -- there's no "current CPU" to
/// resolve, so this is a plain link-time-constant pointer, not a
/// function. Scheduler::init() reads it once per CPU, at boot, on that
/// CPU -- so collection order never matters: every entry carries its own
/// `discipline`, and Scheduler::init() sorts them into an array indexed
/// by discipline itself.
struct ScheduleEntry {
    sched::Discipline discipline;
    ISchedule *instance;
};

/// CRTP base every concrete discipline (Fair, ...) derives from instead
/// of ISchedule directly. `ISchedule` is stateless by design -- all
/// per-CPU scheduling state belongs to the discipline's own per-CPU data
/// (e.g. Fair's RunQueue, held by Scheduler and reached via
/// `Scheduler::fair_run_queue()`), never to the schedule object itself.
/// Because of that, `Derived` needs no per-CPU instance: one shared,
/// stateless instance implements the algorithm for every CPU.
template <typename Derived, sched::Discipline Discipline>
class Schedule: public ISchedule {
  public:
    [[nodiscard]]
    auto discipline() const -> sched::Discipline override final {
        return Discipline;
    }

  private:
    static inline Derived INSTANCE;

    /// The registry entry. Two things about it are easy to get wrong, and
    /// both were:
    ///
    /// The section is `.task.schedules`, not `.init.rodata.schedules`. It is
    /// collected inside kernel.ld's `.init.rodata` output section, which also
    /// ends in a `*(.init.rodata*)` catch-all -- a name under that prefix is
    /// matched by the catch-all first and lands outside its own bounds.
    ///
    /// FORCE_USED is necessary but not sufficient: it keeps the compiler from
    /// eliding the symbol, and says nothing to the linker. The kernel links
    /// with --gc-sections and nothing names this object -- Scheduler::init()
    /// only walks the section bounds -- so kernel.ld has to KEEP it too.
    ///
    /// A third trap lives in fair.cpp: a static data member of a class
    /// template is instantiated only when odr-used, so each discipline needs
    /// an explicit instantiation of its Schedule<> base.
    LINK_SECTION(".task.schedules")
    FORCE_USED
    static inline ScheduleEntry const REGISTRATION = {Discipline, &INSTANCE};
};

} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHEDULE_HPP
