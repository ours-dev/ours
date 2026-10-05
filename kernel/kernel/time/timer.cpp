#include <ours/cpu.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>

namespace ours::time {
auto Timer::activate(Deadline deadline, Handler on_expired) -> void {
    m_canary.verify<Self>();
    TimerQueue::activate_timer(*this, deadline, ustl::move(on_expired));
}

/// Moves a deadline that is behind |now| on by whole intervals until it is
/// ahead of |now|, and answers how many intervals that took. Three cases,
/// cheapest first: not yet due, so nothing to do; one period late, so one
/// step; many periods late, so jump by the whole count and step once more
/// only if that still lands in the past (the division truncates).
auto Timer::forward(TimePoint<> now, Duration interval) -> u64 {
    m_canary.verify<Self>();
    DEBUG_ASSERT(interval > Duration::zero());
    DEBUG_ASSERT(!is_linked(), "forward() on a queued timer would corrupt the queue's order");

    auto expires = m_deadline.when();
    if (expires >= now) {
        return 0;
    }

    auto const delta = now - expires;
    u64 overrun = 1;
    if (delta >= interval) {
        overrun = static_cast<u64>(delta / interval);
        expires += overrun * interval;
        if (expires > now) {
            m_deadline = Deadline::at(expires, m_deadline.slack());
            return overrun;
        }
        overrun += 1;
    }

    expires += interval;
    m_deadline = Deadline::at(expires, m_deadline.slack());
    return overrun;
}

auto Timer::forward(Duration interval) -> u64 {
    return forward(SteadyClock::now(), interval);
}

/// Disarms the timer, and waits out its handler if one is running on another
/// CPU. True when the handler had not started, and now never will.
auto Timer::cancel() -> bool {
    m_canary.verify<Self>();

    auto const cpu = Cpu::id();

    // First, before any lock: a handler spinning in trylock_or_cancel() on
    // the other side of a lock we hold is waiting to see exactly this.
    m_cancelled.store(true, std::memory_order_release);

    // Cancelling from inside our own handler. We cannot wait for the handler
    // to finish -- we are it -- so disarm what we can and report that the
    // handler ran. The queue does not hold us (run_expired_timers() unlinks
    // before calling), so there is nothing to remove.
    if (m_active_cpu.load(std::memory_order_acquire) == cpu) {
        m_on_expired = nullptr;
        return false;
    }

    // Still queued means the handler has not started, and once we unlink
    // under the queue lock it never will. Not queued means it has already
    // finished or is running right now -- the spin below covers that.
    bool handler_not_running = false;
    // The analysis cannot follow the lock across the callback: with_queue_locked()
    // holds queue.m_lock for exactly the duration of this lambda.
    TimerQueue::with_queue_locked(*this, [&](TimerQueue &queue) TA_NO_THREAD_SAFETY_ANALYSIS {
        if (is_linked()) {
            queue.remove_locked(*this);
            handler_not_running = true;
        }
    });

    // Not queued and not on this CPU: either the handler already finished,
    // or it is running on another CPU right now. Wait it out, so that the
    // caller may free the timer -- and everything the handler touches --
    // the moment we return.
    while (m_active_cpu.load(std::memory_order_acquire) != INVALID_CPU_NUM) {
        arch::spin_hint();
    }

    // Defence in depth: a reuse without activate() faults on a null handler
    // instead of running stale code.
    m_on_expired = nullptr;
    return handler_not_running;
}

auto Timer::trylock_or_cancel(sync::SpinLock &lock) -> bool {
    while (!lock.try_acquire()) {
        if (is_cancelled()) {
            return true;
        }
        arch::spin_hint();
    }
    return false;
}

} // namespace ours::time
