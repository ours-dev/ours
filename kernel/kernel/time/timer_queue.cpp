#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer_queue.hpp>

#include <algorithm>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/counter.hpp>
#include <logz4/log.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(TIMER_QUEUE_PROGRAM_REFUSED, "timer.queue.program_refused");

namespace ours::time {
TimerQueue::TimerQueue()
    : m_canary(this)
    , m_cpunum(INVALID_CPU_NUM)
    , m_lock()
    , m_running(nullptr)
    , m_armed_deadline(INFINITE_TIME)
    , m_refused(false)
    , m_bases() {}

INIT_CODE
auto TimerQueue::init(CpuNum cpunum) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);
    DEBUG_ASSERT(m_cpunum == INVALID_CPU_NUM, "TimerQueue of CPU[{}] bound twice", m_cpunum);

    m_cpunum = cpunum;
}

auto TimerQueue::base_of(ClockId clockid) -> ClockBase & {
    DEBUG_ASSERT(clockid < ClockId::MaxNumClocks);
    return m_bases[static_cast<usize>(clockid)];
}

/// The earliest deadline on this queue, in monotonic time.
///
/// A timer's deadline is expressed in its own clock, and the tick device is
/// programmed in monotonic time, so each base's head has to be brought back
/// across that base's offset before the minimum means anything. Every offset
/// is zero today -- Boottime only diverges from Monotonic across a suspend,
/// which does not exist yet -- so this is the expression being right rather
/// than a behaviour change.
auto TimerQueue::earliest_deadline_locked() const -> TimePoint<> {
    TimePoint<> deadline = INFINITE_TIME;
    for (auto const &base: m_bases) {
        if (base.active.empty()) {
            continue;
        }

        auto const expires = base.active.begin()->deadline().latest();
        if (expires == INFINITE_TIME) {
            // A timer armed with no deadline at all. Shifting it would only
            // wrap it back into the representable range.
            continue;
        }

        // Re-wrapped: subtracting a duration yields the std::chrono base type.
        deadline = std::min(TimePoint<>(expires - base.offset), deadline);
    }
    return deadline;
}

auto TimerQueue::reprogram_locked() -> void {
    DEBUG_ASSERT(this == &TimerQueue::get(), "Reprogramming another CPU's tick device");

    auto const deadline = earliest_deadline_locked();
    // Skipped only when the device is known to be armed for exactly this.
    if (deadline == m_armed_deadline) {
        return;
    }

    // In Periodic mode this is a no-op and the tick runs us anyway; in
    // Oneshot mode INFINITE_TIME parks the device. See TickDevice.
    auto const status = TickDevice::Current::get().program_event(deadline);
    if (Status::Ok != status) {
        // The device did not take the deadline, so it may be unarmed or armed
        // for an older one; either way it is not armed for |deadline|, and
        // saying it was would make the next reprogram for the same deadline a
        // no-op. Forgetting what it holds makes that call try again. Nothing
        // here can retry by itself: the next activation, cancellation or
        // expiry on this queue does. A refusal is rare, since the device has
        // already retried at its minimum delta by the time it says no.
        m_armed_deadline = UNKNOWN_DEADLINE;
        TIMER_QUEUE_PROGRAM_REFUSED.add(1);
        m_refused.store(true, std::memory_order_release);
        return;
    }
    m_armed_deadline = deadline;
}

auto TimerQueue::notify_refusal() -> void {
    // The common case, and nothing else to pay for it.
    if (!m_refused.load(std::memory_order_acquire)) {
        return;
    }

    // With interrupts off this CPU cannot change under the check below, and
    // the swap of devices that follows is made with them off, as it requires.
    arch::AutoInterruptDisabler const no_interrupts;
    if (this != &TimerQueue::get()) {
        return;
    }
    if (!m_refused.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    TickDevice::Current::get().device_refused();
}

auto TimerQueue::num_refused_programs() -> usize {
    return static_cast<usize>(TIMER_QUEUE_PROGRAM_REFUSED.sum_across_all_cpus());
}

auto TimerQueue::rearm_device() -> void {
    m_canary.verify<Self>();

    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        m_armed_deadline = UNKNOWN_DEADLINE;
        reprogram_locked();
    }
    notify_refusal();
}

auto TimerQueue::enqueue_locked(Timer &timer) -> void {
    auto &active = base_of(timer.clockid()).active;

    auto const [iter, inserted] = active.insert(timer);
    DEBUG_ASSERT(inserted);

    // Only a new head changes what the device should be armed for.
    if (&*active.begin() == &timer) {
        reprogram_locked();
    }
}

auto TimerQueue::remove_locked(Timer &timer) -> void {
    auto &active = base_of(timer.clockid()).active;

    auto const was_head = &*active.begin() == &timer;
    auto _ = active.erase(active.iterator_to(timer));

    if (was_head && this == &TimerQueue::get()) {
        reprogram_locked();
    }
}

auto TimerQueue::activate_timer(Timer &timer, Deadline deadline, Timer::Handler on_expired)
    -> void {
    auto &self = TimerQueue::get();
    self.m_canary.verify<Self>();

    {
        Guard<sync::SpinLock, IrqSave> guard(self.m_lock);

        ASSERT(!timer.is_linked(), "Timer activated while still queued");

        // Inside our own handler: run_expired_timers() has unlinked us and is
        // waiting for the handler to return, so re-arming is safe -- unless a
        // cancel() raced in, in which case the canceller is about to destroy
        // the timer and the right thing is to do nothing at all. On any other
        // CPU's handler it is a bug: that CPU still owns the timer.
        auto const cpu = Cpu::id();
        auto const active_cpu = timer.m_active_cpu.load(std::memory_order_acquire);
        if (active_cpu == cpu) {
            if (timer.is_cancelled()) {
                return;
            }
        } else {
            ASSERT(active_cpu == INVALID_CPU_NUM, "Timer activated while running on another CPU");
        }

        timer.m_deadline = deadline;
        timer.m_on_expired = ustl::move(on_expired);
        timer.m_cancelled.store(false, std::memory_order_relaxed);
        // m_active_cpu is left alone: run_expired_timers() owns it.
        timer.m_queue.store(&self, std::memory_order_release);

        self.enqueue_locked(timer);
    }

    self.notify_refusal();
}

auto TimerQueue::pop_expired_locked(TimePoint<> now) -> Timer * {
    for (auto &base: m_bases) {
        auto &active = base.active;
        if (active.empty()) {
            continue;
        }

        // |now| is monotonic and the deadline is in this base's clock, so the
        // comparison happens in the base's clock. The mirror of the
        // subtraction in earliest_deadline_locked().
        auto const clock_now = now + base.offset;

        auto &timer = *active.begin();
        if (clock_now < timer.deadline().latest()) {
            continue;
        }

        auto _ = active.erase(active.iterator_to(timer));
        timer.m_active_cpu.store(m_cpunum, std::memory_order_release);
        return &timer;
    }

    return nullptr;
}

/// Runs this CPU's expired timers. Each expired timer is unlinked and marked
/// active under the queue's lock, run with the lock dropped, and marked
/// inactive afterwards; the ordering of those three is the whole cancel()
/// protocol (see Timer), so it is spelled out rather than folded into a
/// helper.
auto TimerQueue::run_expired_timers() -> usize {
    m_canary.verify<Self>();
    DEBUG_ASSERT(this == &TimerQueue::get(), "Running another CPU's timers");

    auto const now = SteadyClock::now();

    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        // The device has fired, so nothing is armed any more; whatever is
        // left after this pass gets programmed afresh at the end.
        m_armed_deadline = INFINITE_TIME;
    }

    usize num_expired = 0;
    for (;;) {
        Timer *timer = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard(m_lock);
            timer = pop_expired_locked(now);
            if (!timer) {
                break;
            }
            // From here until the lock is retaken below, this CPU is inside
            // the handler; the flag goes up in the same critical section
            // that hands the timer out so there is no instant where the
            // handler runs unrecorded.
            m_running = timer;
        }
        num_expired += 1;

        // Lock dropped, interrupts still off (we are the tick). Releasing the
        // lock above is the release fence that publishes m_active_cpu.
        auto const result = timer->on_expired();

        {
            Guard<sync::SpinLock, IrqSave> guard(m_lock);
            m_running = nullptr;

            if (result == Timer::HandlerResult::Restart && !timer->is_cancelled()) {
                // The handler forwarded the deadline and wants to go back in.
                // It is still marked active on this CPU, which is the state
                // activate_timer() recognises as "re-arming from the handler";
                // a cancel() that raced the handler wins and the timer stays
                // out.
                ASSERT(!timer->is_linked(), "Restarted timer re-armed itself as well");
                enqueue_locked(*timer);
            }
        }

        // Handler done: a cancel() spinning on this may now free the timer.
        timer->m_active_cpu.store(INVALID_CPU_NUM, std::memory_order_release);
    }

    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        reprogram_locked();
    }
    notify_refusal();
    return num_expired;
}

} // namespace ours::time
