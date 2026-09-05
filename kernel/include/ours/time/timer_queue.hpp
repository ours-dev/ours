/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_QUEUE_HPP
#define OURS_TIME_TIMER_QUEUE_HPP 1

#include <arch/spin_hint.hpp>
#include <gktl/canary.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/timer.hpp>
#include <ustl/collections/intrusive/set.hpp>

namespace ours::time {
/// The per-CPU timer engine: every CPU owns exactly one, reached through
/// `TimerQueue::get()`, and a `Timer` is always queued on the CPU that
/// activated it.
///
/// There is deliberately no scheduler layer above this class. An earlier
/// design had a `TimerScheduler` holding one `TimerQueue` per clock and
/// forwarding `activate`/`run` to them; that indirection carried no state of
/// its own, so it was folded in here and the per-clock split became the
/// private `ClockBase` array below. This is the shape both references settle
/// on: Zircon's `TimerQueue` is the per-CPU object with nothing above it, and
/// Linux's `hrtimer_cpu_base` embeds its `hrtimer_clock_base[]` the same way.
///
/// Locking. One spin lock per queue, taken with interrupts off, covers the
/// timer sets and the programmed deadline. Zircon uses a single global
/// TimerLock instead; a per-CPU lock is Linux's choice (hrtimer_cpu_base
/// ::lock), and the price of it is that a Timer must be able to find and
/// lock the queue it is on from another CPU -- lock_queue_of() does that,
/// the way Linux's lock_hrtimer_base() does. Handlers run with the lock
/// *released* (run_expired_timers() drops it around each one), which is what
/// lets a handler re-arm itself or cancel another timer.
class TimerQueue: public mm::CpuLocal::Current<TimerQueue> {
    using Self = TimerQueue;

  public:
    TimerQueue();

    ~TimerQueue() = default;

    /// Binds this queue to the CPU whose per-CPU unit it lives in.
    ///
    /// The constructor cannot do this: it runs once, on the template copy that
    /// `CpuLocal::init()` later replicates into every unit, so nothing it
    /// records could be CPU-specific. `time::init()` walks the possible CPUs
    /// from the BSP and calls this on each unit -- the same pattern as
    /// `task::init()` binding every `Scheduler` -- rather than waiting for the
    /// AP to come up, because a timer may be activated *for* a CPU before that
    /// CPU has run a single instruction.
    INIT_CODE
    auto init(CpuNum cpunum) -> void;

    /// Runs every timer on this CPU whose deadline has passed, across all
    /// clocks, then reprograms the tick device for the earliest one left.
    /// Called from the tick, in interrupt context, on this CPU only.
    ///
    /// Returns how many timers expired.
    auto run_expired_timers() -> usize;

    [[nodiscard]]
    FORCE_INLINE
    auto cpunum() const -> CpuNum {
        return m_cpunum;
    }

  private:
    friend class Timer;

    /// One clock's worth of timers, in the sense of Linux's
    /// `hrtimer_clock_base`: the timers themselves plus the offset that maps
    /// this clock onto the monotonic time the tick device is programmed in.
    struct ClockBase {
        ustl::collections::intrusive::Set<Timer> active;

        /// Offset = ClockTime - MonotonicTime, so a deadline held in this
        /// clock becomes monotonic by subtracting it and a monotonic instant
        /// becomes this clock's by adding it.
        ///
        /// Zero for every clock today, and nothing writes it. Monotonic's is
        /// zero by definition; Boottime's would become non-zero the first time
        /// the machine suspends, since boot time keeps counting across a
        /// suspend and monotonic time does not. That is where a setter
        /// belongs when suspend arrives.
        Duration offset{};
    };

    /// Arms |timer| on the current CPU's queue with |deadline| and
    /// |on_expired|. The whole of Zircon's Timer::Set() after its argument
    /// checks; see Timer::activate() for the contract.
    static auto activate_timer(Timer &timer, Deadline deadline, Timer::Handler on_expired) -> void;

    /// Runs |fn(queue)| with the lock of the queue |timer| is on held and
    /// interrupts off, for a caller that may not be on that queue's CPU.
    ///
    /// The problem this solves is "which lock?". The lock is per queue, one
    /// per CPU, and a timer is not necessarily cancelled where it was armed:
    /// a thread sleeps on CPU 0, so its timer sits on CPU 0's queue; it is
    /// woken early, migrated, and calls cancel() from CPU 1. The lock to take
    /// is CPU 0's, and the only thing that says so is timer.m_queue.
    ///
    /// But m_queue is written under the queue lock (activate_timer()), and
    /// here it has to be read *before* holding any lock -- that is the whole
    /// point, we do not yet know which lock. So the value read may be stale:
    /// it says "CPU 0", we take CPU 0's lock, and meanwhile someone on CPU 2
    /// has activated the timer, which is now on CPU 2's queue; holding CPU
    /// 0's lock protects nothing about a node in CPU 2's tree. Hence the
    /// loop: take the lock the pointer names, then read the pointer again
    /// under it. If it still names this queue, the timer cannot move --
    /// moving requires this lock -- and |fn| is safe. If not, drop the lock
    /// and try the new value.
    ///
    /// This is Linux's lock_hrtimer_base(), with the caller's work passed in
    /// rather than the lock passed out, so the lock never outlives a scope.
    /// The alternative is Zircon's: one global TimerLock for every CPU's
    /// queue, under which cancel() can unlink from anywhere without asking
    /// where. That removes this function and the second load, at the cost of
    /// all CPUs serialising their timer operations on one lock. The per-CPU
    /// lock was chosen to match the rest of this subsystem's Linux shape;
    /// switching is local to this file.
    ///
    /// |fn| is not called at all for a timer that has never been activated:
    /// m_queue is only ever set, never cleared, so null means "never been
    /// anywhere", not "not currently queued" -- is_linked() answers that, and
    /// only from inside |fn|.
    template <typename F>
    TA_NO_THREAD_SAFETY_ANALYSIS static auto with_queue_locked(Timer &timer, F &&fn) -> void {
        for (;;) {
            auto *const queue = timer.m_queue.load(std::memory_order_acquire);
            if (!queue) {
                return;
            }

            {
                Guard<sync::SpinLock, IrqSave> guard(queue->m_lock);
                if (queue == timer.m_queue.load(std::memory_order_relaxed)) {
                    fn(*queue);
                    return;
                }
            }

            // Moved to another CPU's queue between the load and the lock.
            arch::spin_hint();
        }
    }

    TA_REQUIRES(m_lock)
    auto base_of(ClockId clockid) -> ClockBase &;

    /// Links an armed timer into its clock's set and, if it became the
    /// earliest thing on this queue, reprograms the tick device.
    TA_REQUIRES(m_lock)
    auto enqueue_locked(Timer &timer) -> void;

    /// Unlinks a queued timer. Reprograms the tick device only when this is
    /// the current CPU's queue: another CPU's device is left to fire at the
    /// stale deadline and find nothing to do, which is cheaper than an IPI
    /// to tell it (Zircon's Cancel() makes the same trade).
    TA_REQUIRES(m_lock)
    auto remove_locked(Timer &timer) -> void;

    /// The earliest deadline across all clocks, or INFINITE_TIME.
    TA_REQUIRES(m_lock)
    [[nodiscard]]
    auto earliest_deadline_locked() const -> TimePoint<>;

    /// Unlinks and returns the earliest timer due at or before |now| on any
    /// clock, or null when nothing is due. The timer comes back marked as
    /// owned by this CPU (m_active_cpu) -- not "running" yet, but claimed,
    /// so that a cancel() arriving between the caller dropping the lock and
    /// calling the handler sees it as busy and waits, rather than as
    /// finished and freeable.
    TA_REQUIRES(m_lock)
    auto pop_expired_locked(TimePoint<> now) -> Timer *;

    /// Programs the tick device for earliest_deadline_locked() if that is
    /// not what it is already armed for. Only for the current CPU's queue.
    TA_REQUIRES(m_lock)
    auto reprogram_locked() -> void;

    gktl::Canary m_canary;
    CpuNum m_cpunum;

    sync::SpinLock m_lock;

    /// The timer whose handler this CPU is executing right now, or null --
    /// Linux's hrtimer_cpu_base::running. Set by run_expired_timers() in the
    /// same critical section that hands the timer out and cleared in the one
    /// that takes it back, so it is exact: non-null exactly while the
    /// handler is on this CPU's stack. It is not what cancel() waits on
    /// (that is Timer::m_active_cpu, which marks ownership from the moment
    /// of unlinking); it is the queue's own view, for "is the callback
    /// running on this CPU" questions.
    Timer *m_running TA_GUARDED(m_lock);

    /// The deadline the tick device is currently programmed for, so that
    /// inserting a later timer does not reprogram it needlessly.
    TimePoint<> m_next_deadline TA_GUARDED(m_lock);

    ClockBase m_bases[static_cast<usize>(ClockId::MaxNumClocks)] TA_GUARDED(m_lock);
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER_QUEUE_HPP
