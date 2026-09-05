/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_SCHED_DRIVER_HPP
#define OURS_TASK_SCHED_DRIVER_HPP 1

#include <ours/compiler.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/sched/types.hpp>

namespace ours::task::sched {
class Driver {
    using Self = Driver;

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

    auto reset_preempt(SchedDuration delta) -> void;

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

#endif // #ifndef OURS_TASK_SCHED_DRIVER_HPP