/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_QUEUE_HPP
#define OURS_TIME_TIMER_QUEUE_HPP 1

#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/timer.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <gktl/canary.hpp>
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
/// private `ClockBase` array below.
///
/// Locking. One spin lock per queue, taken with interrupts off, covers the
/// timer sets and the programmed deadline. The price of a lock per queue
/// rather than one for all of them is that a Timer must be able to find and
/// lock the queue it is on from another CPU; with_queue_locked() does that.
/// Handlers run with the lock *released* (run_expired_timers() drops it
/// around each one), which is what lets a handler re-arm itself or cancel
/// another timer.
///
/// Besides its timers the queue holds one deadline that is not a timer: the
/// end of the running thread's slice (reset_preempt()). The device is armed
/// for the earlier of it and the queue's head, so the queue stays the one
/// writer of the device, and nothing ticks to find out whether a slice is
/// over. The scheduler writes it under its run queue lock, so that lock comes
/// before this one.
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
    /// A preemption deadline that has passed is taken first: the slot is
    /// emptied and the scheduler told (Scheduler::Current::on_preempt_timer()),
    /// which asks for a reschedule at the interrupt's exit; the reschedule sets
    /// the next one.
    ///
    /// Returns how many timers expired.
    auto run_expired_timers() -> usize;

    /// Sets this CPU's preemption deadline, the end of the running thread's
    /// slice, and arms the device for it if it is now the earliest thing on
    /// the queue; INFINITE_TIME empties the slot, for a CPU running its idle
    /// thread. Called by the scheduler each time it picks a thread
    /// (Scheduler::set_next_thread()), on this CPU, with interrupts off and
    /// its run queue lock held.
    ///
    /// A refusal by the device is counted and left for the next visit to this
    /// queue (notify_refusal()): its answer takes locks of its own, which
    /// must not nest inside the run queue lock.
    auto reset_preempt(TimePoint<> deadline) -> void;

    /// Arms the tick device for the earliest deadline from scratch, forgetting
    /// what it was thought to be armed for. For the moment the device goes
    /// from a mode that ignores the queue's deadlines (periodic, where
    /// programming is a no-op) to one that follows them (one-shot): the
    /// deadline recorded so far was never written to the hardware, and the
    /// ordinary reprogram would skip it as already armed. On this CPU's queue
    /// only.
    auto rearm_device() -> void;

    /// rearm_device() that does not pass a refusal on: arms the device from
    /// scratch and answers whether it took the deadline. For the one caller
    /// that is itself the handler of a refusal (TickDevice::on_device_refused()),
    /// which asks again and decides what a second refusal means. A refusal is
    /// still counted. On this CPU's queue only.
    [[nodiscard]]
    auto try_rearm_device() -> bool;

    /// Moves every timer on |cpu|'s queue onto the calling CPU's, and arms
    /// this CPU's device for the earliest: |cpu| has gone offline, and a
    /// timer left on its queue would never fire. Called on any other CPU
    /// once |cpu| has stopped (TickDevice::stop_for_offline() cancelled its
    /// tick), so nothing runs |cpu|'s timers or arms new ones on its queue
    /// meanwhile; a timer on it may still be cancelled from any CPU, which
    /// takes |cpu|'s lock, held here throughout, so a timer is on one queue
    /// or the other at every moment.
    static auto take_over_from(CpuNum cpu) -> void;

    /// How many times a tick device has refused a deadline since boot, over
    /// all CPUs. Zero in a healthy run; for tests and diagnosis.
    [[nodiscard]]
    static auto num_refused_programs() -> usize;

    /// How many times |cpu|'s preemption deadline has passed since boot: the
    /// slices that ran to their end there. Zero on a CPU that stayed idle,
    /// whose idle thread has no slice; for tests (preempt.test).
    [[nodiscard]]
    static auto num_preempts(CpuNum cpu) -> u64;

    /// How many times |cpu| has run its expired timers since boot: once per
    /// timer interrupt it took, whether from its own device, in either mode,
    /// or from a broadcast device's IPI. What an idle CPU is woken for, and
    /// so the first thing measured about idle power (idle.stats).
    [[nodiscard]]
    static auto num_runs(CpuNum cpu) -> u64;

    [[nodiscard]]
    FORCE_INLINE
    auto cpunum() const -> CpuNum {
        return m_cpunum;
    }

  private:
    friend class Timer;

    /// One clock's worth of timers: the timers themselves plus the offset
    /// that maps this clock onto the monotonic time the tick device is
    /// programmed in.
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

    /// Stands in m_armed_deadline for "the device may hold anything": a
    /// deadline no timer can usefully have, as INFINITE_TIME is one at the
    /// other end of the range.
    static constexpr auto UNKNOWN_DEADLINE = TimePoint<>::min();

    /// Arms |timer| on the current CPU's queue with |deadline| and
    /// |on_expired|. Timer::activate() only forwards here, so this is the
    /// whole of arming a timer; see Timer::activate() for the contract.
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
    /// The caller's work is passed in rather than the lock passed out, so the
    /// lock never outlives a scope. The alternative is one global lock for
    /// every CPU's queue, under which cancel() can unlink from anywhere
    /// without asking where. That removes this function and the second load,
    /// at the cost of all CPUs serialising their timer operations on one
    /// lock. The per-CPU lock was chosen to match the rest of this subsystem,
    /// whose tick state is per CPU throughout (TickDevice, TickSched);
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
    /// to tell it.
    TA_REQUIRES(m_lock)
    auto remove_locked(Timer &timer) -> void;

    /// The earliest deadline across all clocks and the preemption deadline,
    /// or INFINITE_TIME.
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

    /// Programs the tick device for earliest_deadline_locked() unless it is
    /// known to be armed for exactly that already. A device that refuses is
    /// counted, leaves m_armed_deadline unknown, so the next call tries
    /// again, and raises m_refused for notify_refusal(). Only for the current
    /// CPU's queue.
    TA_REQUIRES(m_lock)
    auto reprogram_locked() -> void;

    /// Tells the tick device that it refused a deadline, if it did since the
    /// last time. Called by whoever reprogrammed, after dropping the lock:
    /// the answer is a device swap, which takes locks of its own and must not
    /// run under this one. A caller that has migrated since it reprogrammed
    /// leaves the flag for the next visit to this queue's CPU.
    auto notify_refusal() -> void;

    gktl::Canary m_canary;
    CpuNum m_cpunum;

    sync::SpinLock m_lock;

    /// The timer whose handler this CPU is executing right now, or null. Set
    /// by run_expired_timers() in the same critical section that hands the
    /// timer out and cleared in the one that takes it back, so it is exact:
    /// non-null exactly while the handler is on this CPU's stack. It is not
    /// what cancel() waits on (that is Timer::m_active_cpu, which marks
    /// ownership from the moment of unlinking); it is the queue's own view,
    /// for "is the callback running on this CPU" questions.
    Timer *m_running TA_GUARDED(m_lock);

    /// What the tick device is armed for, as far as this queue knows, so that
    /// inserting a later timer does not reprogram it needlessly: a deadline;
    /// INFINITE_TIME for "nothing armed"; or UNKNOWN_DEADLINE, which is what
    /// a programming the device refused leaves. It must not name a deadline
    /// the device never took -- the next reprogram for that same deadline
    /// would then see nothing to do, and a one-shot CPU would never be
    /// interrupted for it.
    TimePoint<> m_armed_deadline TA_GUARDED(m_lock);

    /// When the running thread's slice ends, or INFINITE_TIME while this CPU
    /// runs its idle thread. Written by reset_preempt() and emptied by
    /// run_expired_timers() once it has passed, both on this CPU; under the
    /// lock because reprogram_locked() reads it with the timers.
    TimePoint<> m_preempt_deadline TA_GUARDED(m_lock);

    /// Raised under the lock by a refused programming and taken, with no lock
    /// held, by notify_refusal(). An atomic because the two are not under the
    /// same lock hold, so that a thread and the interrupt that lands in the
    /// gap between them see one flag. Both run on this queue's CPU, and the
    /// flag publishes nothing to another, so the accesses are relaxed.
    std::atomic<bool> m_refused;

    ClockBase m_bases[static_cast<usize>(ClockId::MaxNumClocks)] TA_GUARDED(m_lock);
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER_QUEUE_HPP
