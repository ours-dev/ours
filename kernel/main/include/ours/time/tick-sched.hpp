/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_SCHED_HPP
#define OURS_TIME_TICK_SCHED_HPP 1

#include <ours/time/timer.hpp>
#include <ours/mem/cpu-local.hpp>
#include <ours/syscall/time.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::time {
    /// Emulates a tick device, which generates periodic tick events for scheduler.
    class TickSched: public mem::CpuLocal::Current<TickSched> {
        using Self = TickSched;
      public:
        enum class WorkMode {
            Shutdown,

            // Legacy periodic tick mode, frequency dependent on the OURS_CONFIG_HZ
            Legacy,

            // High precision tick mode, based on high precision clock device. 
            HpTick,
        };

        TickSched();

        ~TickSched() = default;

        /// Start to emulate the device which periodically generates a tick.
        auto emulate(WorkMode workmode) -> void;

        FORCE_INLINE
        auto is_legacy() const -> bool {
            return m_workmode == WorkMode::Legacy;
        }

        FORCE_INLINE
        auto is_hptick() const -> bool {
            return m_workmode == WorkMode::HpTick;
        }
      private:
        friend class TickDevice;
        static auto handle_event(Timer *) -> Timer::HandlerResult;

        gktl::Canary m_canary;
        Timer m_timer;
        WorkMode m_workmode;
    };

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_SCHED_HPP