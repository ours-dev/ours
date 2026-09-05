/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_TASK_AUTO_PREEMPT_DISABLER_HPP
#define OURS_TASK_AUTO_PREEMPT_DISABLER_HPP 1

#include <ours/task/thread.hpp>

namespace ours::task {
/// Holds off preemption of the current thread on this CPU for a scope.
///
/// While one of these is alive, a reschedule that becomes due is only
/// *recorded* -- PreemptionState::add_pending() notes which CPUs want one --
/// and is delivered when the last disabler goes out of scope. Interrupt
/// handlers are included: an interrupt may arrive and make a thread
/// runnable, but it will not switch away from the current one.
///
/// It does **not** make the scope non-blocking. A thread that sleeps, or
/// takes a contended mutex, still switches away with preemption disabled.
/// This guards against being switched away *involuntarily*, and nothing
/// more; if a region must not block either, that is a separate concern.
///
/// Requests to *other* CPUs are still delivered immediately -- see
/// AutoEagerReschedDisabler below for the stronger form that batches those
/// too.
///
/// Pass `Defer` to construct one without disabling anything yet, and call
/// disable() later.
class AutoPreemptDisabler {
  public:
    enum DeferType { Defer };

    /// |m_disabled| means "this object is currently holding preemption
    /// off", so it starts false and disable() is what turns it on. It used
    /// to start true and call enable(), which did the exact opposite of
    /// what the class is named: constructing one *enabled* preemption, and
    /// left m_disabled false so the destructor did nothing at all.
    FORCE_INLINE
    AutoPreemptDisabler()
        : m_disabled(false) {
        disable();
    }

    FORCE_INLINE
    AutoPreemptDisabler(DeferType defer)
        : m_disabled(false) {}

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

/// The strictly stronger form of AutoPreemptDisabler: holds off delivering
/// reschedules to *every* CPU, not just this one.
///
/// Local preemption is suppressed exactly as above, and in addition no
/// reschedule IPI is sent to any remote CPU that becomes due during the
/// scope. Both kinds accumulate in PreemptionState's pending mask and go
/// out together when the count drops to zero -- which is the point: an
/// interrupt handler that wakes several threads across several CPUs would
/// otherwise fire one IPI per wakeup instead of one IPI per CPU. The IRQ
/// path (irq/mod.cpp) wraps every handler in exactly this pair.
///
/// Because it covers everything the preempt disable count covers, holding
/// this implies holding that; Zircon states the relationship directly in
/// EagerReschedReenable ("eager resched disable implies preempt disable")
/// and returns early rather than flushing when the eager count is still
/// non-zero.
///
/// As with AutoPreemptDisabler, blocking operations remain allowed.
class AutoEagerReschedDisabler {
  public:
    enum DeferType { Defer };

    /// Same correction as AutoPreemptDisabler above. Starting true made
    /// disable()'s `if (!m_disabled)` guard fall through, so nothing was
    /// ever disabled -- while the destructor still called enable(),
    /// re-enabling eager rescheduling that this object never took away.
    FORCE_INLINE
    AutoEagerReschedDisabler()
        : m_disabled(false) {
        disable();
    }

    FORCE_INLINE
    AutoEagerReschedDisabler(DeferType)
        : m_disabled(false) {}

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