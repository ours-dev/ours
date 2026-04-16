/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_TASK_AUTO_PREEMPT_DISABLER_HPP
#define OURS_TASK_AUTO_PREEMPT_DISABLER_HPP 1

#include <ours/task/thread.hpp>

namespace ours::task {
    class AutoPreemptDisabler {
      public:
        enum DeferType { Defer };

        FORCE_INLINE
        AutoPreemptDisabler() 
            : m_disabled(true) {
            enable();
        }

        FORCE_INLINE
        AutoPreemptDisabler(DeferType defer)
            : m_disabled(false) {
        }

        USTL_NO_MOVEABLE_AND_COPYABLE(AutoPreemptDisabler);

        FORCE_INLINE
        ~AutoPreemptDisabler() {
            enable();
        }

        FORCE_INLINE
        auto enable() -> void {
            if (m_disabled) {
                m_disabled = false;
                Thread::Current::preemption_state().enable_preemption();
            }
        }

        FORCE_INLINE
        auto disable() -> void {
            if (!m_disabled) {
                m_disabled = true;
                Thread::Current::preemption_state().disable_preemption();
            }
        }
      private:
        bool m_disabled = false;
    };

    class AutoEagerReschedDisabler {
      public:
        enum DeferType { Defer };

        FORCE_INLINE
        AutoEagerReschedDisabler() 
            : m_disabled(true) {
            disable();
        }

        FORCE_INLINE
        AutoEagerReschedDisabler(DeferType) 
            : m_disabled(false) {
        }

        USTL_NO_MOVEABLE_AND_COPYABLE(AutoEagerReschedDisabler);

        FORCE_INLINE
        ~AutoEagerReschedDisabler() {
            enable();
        }

        FORCE_INLINE
        auto enable() -> void {
            if (m_disabled) {
                m_disabled = false;
                Thread::Current::preemption_state().enable_eager_reschedule();
            }
        }

        FORCE_INLINE
        auto disable() -> void {
            if (!m_disabled) {
                m_disabled = true;
                Thread::Current::preemption_state().disable_eager_reschedule();
            }
        }       
      private:
        bool m_disabled = false;
    };

} // namespace ours::task

#endif // #ifndef OURS_TASK_AUTO_PREEMPT_DISABLER_HPP