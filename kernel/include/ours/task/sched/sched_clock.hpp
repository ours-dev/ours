/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_SCHED_CLOCK_HPP
#define OURS_TASK_SCHED_SCHED_CLOCK_HPP 1

#include <ours/compiler.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/sched/types.hpp>

namespace ours::task::sched {
/// This CPU's scheduler clock: what time it is for scheduling decisions, and
/// when to be interrupted next.
///
/// It was called `Driver`, which in a kernel means a device driver and so
/// named none of this. The rename is not only tidiness: the class holds two
/// clocks -- the scheduler's own and the last real reading it advanced from --
/// of the same type and with similar names, and a version of this file crossed
/// them, storing the scheduler clock where the real reading belonged. The
/// result tracked real time correctly on average and jittered by a whole tick
/// within every six updates, which is the kind of defect a name that says
/// nothing helps to hide.
///
/// The scheduler clock is a *freezable* view of the steady clock: it advances
/// by real elapsed time, except across an update the caller asked to skip,
/// whose interval is folded into the next one rather than lost.
///
/// reset_preempt() and cancel_preempt() are the members that are not about
/// telling the time. They belong here because "when should this CPU be
/// interrupted next" is asked and answered in the same breath as "what time
/// is it". They are also the members to look at first if this class is ever
/// split.
class SchedClock {
    using Self = SchedClock;

  public:
    auto update_clock() -> SchedTime;

    auto request_skip_next_clock_update() -> void {
        m_clock_flags |= ClockFlags::ReqSkip;
    }

    auto accept_skip_next_clock_update() -> void {
        if (!!(m_clock_flags & ClockFlags::ReqSkip)) {
            m_clock_flags = ClockFlags::ActSkip;
        }
    }

    auto note_clock_updated() -> void {
        m_clock_flags = ClockFlags::Updated;
    }

    /// This CPU's preemption deadline, |delta| past the clock's last real
    /// reading, in its timer queue (TimerQueue::reset_preempt()). For a pick,
    /// which has just updated the clock. On this CPU, interrupts off.
    auto reset_preempt(SchedDuration delta) -> void;

    /// No preemption deadline on this CPU: it runs its idle thread.
    auto cancel_preempt() -> void;

    FORCE_INLINE
    auto clock() const -> SchedTime {
        return m_sched_clock;
    }

  private:
    enum class ClockFlags {
        ReqSkip = 0x01,
        ActSkip = 0x02,
        Updated = 0x04,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(ClockFlags);

    SchedTime m_sched_clock;
    SchedTime m_prev_real_clock;
    ClockFlags m_clock_flags;
};
} // namespace ours::task::sched

#endif // #ifndef OURS_TASK_SCHED_SCHED_CLOCK_HPP