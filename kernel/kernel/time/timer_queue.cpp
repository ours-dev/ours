#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer_queue.hpp>

#include <algorithm>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/counter.hpp>
#include <logz4/log.hpp>

// NOLINTBEGIN(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(TIMER_QUEUE_PROGRAM_REFUSED, "timer.queue.program_refused");
GKTL_COUNTER(TIMER_QUEUE_RUNS, "timer.queue.runs");
GKTL_COUNTER(TIMER_QUEUE_PREEMPTS, "timer.queue.preempts");
GKTL_COUNTER(TIMER_QUEUE_HELD_PASSES, "timer.queue.held_passes");
GKTL_COUNTER(TIMER_QUEUE_HELD_OFF_NS, "timer.queue.held_off_ns");
// NOLINTEND(readability-identifier-naming)

namespace ours::time {
namespace {
    /// The most a pass can hold the device off for, whatever it took: a
    /// callback that ran for a second does not leave the CPU's other timers a
    /// second late as well.
    constexpr Duration MAX_HOLD_OFF = Milliseconds(100);

    /// TimerQueue::m_hold_off_until while nothing holds the device off.
    constexpr TimePoint<> NO_HOLD_OFF = TimePoint<>::min();
} // namespace

TimerQueue::TimerQueue()
    : m_canary(this)
    , m_cpunum(INVALID_CPU_NUM)
    , m_lock()
    , m_running(nullptr)
    , m_armed_deadline(INFINITE_TIME)
    , m_preempt_deadline(INFINITE_TIME)
    , m_hold_off_until(NO_HOLD_OFF)
    , m_refused(false)
    , m_bases() {}

INIT_CODE
auto TimerQueue::init(CpuNum cpunum) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);
    DEBUG_ASSERT(m_cpunum == INVALID_CPU_NUM, "TimerQueue of CPU[{}] bound twice", m_cpunum);

    m_cpunum = cpunum;

    // A list's header points at itself, and this unit is a copy of the
    // template the constructor ran on: its header still points at the
    // template's, so the list looks full. Built again here, in place and not
    // assigned, since the copy has nothing of its own to give up. The sets
    // are empty with no pointer to themselves and survive the copy.
    new (&m_restart) Timer::RestartList();
}

auto TimerQueue::base_of(ClockId clockid) -> ClockBase & {
    DEBUG_ASSERT(clockid < ClockId::MaxNumClocks);
    return m_bases[static_cast<usize>(clockid)];
}

/// The earliest deadline on this queue, in monotonic time: the timers' and
/// the preemption deadline.
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

    // The timers wait for the hold-off, the end of a slice does not. With no
    // timer the deadline is INFINITE_TIME and stays so: nothing is later.
    deadline = std::max(deadline, m_hold_off_until);
    return std::min(deadline, m_preempt_deadline);
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
        m_refused.store(true, std::memory_order_relaxed);
        return;
    }
    m_armed_deadline = deadline;
}

auto TimerQueue::notify_refusal() -> void {
    // The common case, and nothing else to pay for it.
    if (!m_refused.load(std::memory_order_relaxed)) {
        return;
    }

    // With interrupts off this CPU cannot change under the check below, and
    // the swap of devices that follows is made with them off, as it requires.
    arch::AutoInterruptDisabler const no_interrupts;
    if (this != &TimerQueue::get()) {
        return;
    }
    if (!m_refused.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    TickDevice::Current::get().on_device_refused();
}

auto TimerQueue::num_refused_programs() -> usize {
    return static_cast<usize>(TIMER_QUEUE_PROGRAM_REFUSED.sum_across_all_cpus());
}

auto TimerQueue::num_held_passes(CpuNum const cpu) -> u64 {
    return static_cast<u64>(TIMER_QUEUE_HELD_PASSES.get(cpu));
}

auto TimerQueue::held_off_ns(CpuNum const cpu) -> u64 {
    return static_cast<u64>(TIMER_QUEUE_HELD_OFF_NS.get(cpu));
}

auto TimerQueue::num_preempts(CpuNum const cpu) -> u64 {
    return static_cast<u64>(TIMER_QUEUE_PREEMPTS.get(cpu));
}

auto TimerQueue::num_runs(CpuNum const cpu) -> u64 {
    return static_cast<u64>(TIMER_QUEUE_RUNS.get(cpu));
}

auto TimerQueue::reset_preempt(TimePoint<> const deadline) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(this == &TimerQueue::get(), "Setting another CPU's preemption deadline");

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    m_preempt_deadline = deadline;
    reprogram_locked();
}

auto TimerQueue::armed_deadline() -> TimePoint<> {
    m_canary.verify<Self>();
    DEBUG_ASSERT(this == &TimerQueue::get(), "Asking another CPU's queue what its device holds");

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    return m_armed_deadline;
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

auto TimerQueue::try_rearm_device() -> bool {
    m_canary.verify<Self>();

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    m_armed_deadline = UNKNOWN_DEADLINE;
    reprogram_locked();
    // Taken back here and not left for notify_refusal(): the caller has the
    // answer, and a flag left up would send it a second time.
    return !m_refused.exchange(false, std::memory_order_relaxed);
}

/// The dying queue's lock, then this one's. Nothing else holds two queue
/// locks at once -- with_queue_locked() and every queue operation take one --
/// so the order cannot meet its reverse. Interrupts off for the whole move,
/// so this CPU's queue is the one reprogrammed and asked about a refusal.
///
/// A timer keeps its deadline and handler; only the queue it names changes,
/// under both locks. Its m_active_cpu is INVALID_CPU_NUM, since only a
/// timer popped by run_expired_timers() has another, and |cpu| runs none.
auto TimerQueue::take_over_from(CpuNum const cpu) -> void {
    arch::AutoInterruptDisabler const no_interrupts;
    auto &self = TimerQueue::get();
    auto &from = TimerQueue::get(cpu);
    DEBUG_ASSERT(&self != &from, "CPU[{}] taking over its own timers", cpu);
    self.m_canary.verify<Self>();
    from.m_canary.verify<Self>();

    {
        Guard<sync::SpinLock, IrqSave> from_guard(from.m_lock);
        Guard<sync::SpinLock, IrqSave> guard(self.m_lock);
        DEBUG_ASSERT(from.m_running == nullptr, "CPU[{}] went offline in a timer's handler", cpu);
        DEBUG_ASSERT(from.m_restart.empty(), "CPU[{}] went offline in a timer pass", cpu);
        DEBUG_ASSERT(from.m_preempt_deadline == INFINITE_TIME,
                     "CPU[{}] went offline in a thread other than its idle thread", cpu);
        for (usize i = 0; i < static_cast<usize>(ClockId::MaxNumClocks); ++i) {
            auto &source = from.m_bases[i].active;
            auto &target = self.m_bases[i].active;
            while (!source.empty()) {
                auto &timer = *source.begin();
                auto _ = source.erase(source.iterator_to(timer));
                timer.m_queue.store(&self, std::memory_order_release);
                auto const [iter, inserted] = target.insert(timer);
                DEBUG_ASSERT(inserted);
            }
        }
        from.m_armed_deadline = INFINITE_TIME;
        self.m_armed_deadline = UNKNOWN_DEADLINE;
        self.reprogram_locked();
    }
    self.notify_refusal();
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

auto TimerQueue::unpark_locked(Timer &timer) -> void {
    DEBUG_ASSERT(timer.m_parked);
    auto _ = m_restart.erase(m_restart.iterator_to(timer));
    timer.m_parked = false;
}

auto TimerQueue::activate_timer(Timer &timer, Deadline deadline, Timer::Handler on_expired)
    -> void {
    // This CPU from the choice of the queue to the notify at the end. A thread
    // may be preempted and move to another CPU at any point it is not
    // disabled for, and then it would lock the queue of the CPU it left,
    // reprogram the device of the CPU it is on for that queue's deadline, and
    // ask the wrong queue whether its device refused.
    arch::AutoInterruptDisabler const no_interrupts;
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
    TIMER_QUEUE_RUNS.add(1);

    auto const now = SteadyClock::now();

    auto slice_over = false;
    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        // The device has fired, so nothing is armed any more; whatever is
        // left after this pass gets programmed afresh at the end.
        m_armed_deadline = INFINITE_TIME;
        // Emptied here, not left for the scheduler: until the reschedule
        // sets the next one, a slot still in the past would arm the device
        // for "now" at the end of this pass, and again after every interrupt.
        slice_over = now >= m_preempt_deadline;
        if (slice_over) {
            m_preempt_deadline = INFINITE_TIME;
        }
    }
    if (slice_over) {
        TIMER_QUEUE_PREEMPTS.add(1);
        task::Scheduler::Current::on_preempt_timer();
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
                //
                // It goes into the set at the end of the pass, not here: put
                // in here it would become the head, and the device would be
                // armed for it now and again, for the same timer, at the end.
                // Until then it is parked in m_restart, which cancel() knows.
                ASSERT(!timer->is_linked(), "Restarted timer re-armed itself as well");
                m_restart.push_front(*timer);
                timer->m_parked = true;
            }
        }

        // Handler done: a cancel() spinning on this may now free the timer.
        timer->m_active_cpu.store(INVALID_CPU_NUM, std::memory_order_release);
    }

    // A pass that ran a callback holds the device off for as long as it took.
    // Without that, a periodic timer whose callback is slower than its period
    // is due again within one period of the pass's end, the device interrupts
    // as soon as it can, and the thread it interrupts is left the gap between
    // two callbacks and nothing more. The clock is read before the lock is
    // taken. A pass that ran nothing leaves an earlier hold-off as it is.
    TimePoint<> hold_off_until = NO_HOLD_OFF;
    if (num_expired != 0) {
        auto const end = SteadyClock::now();
        auto const held = std::min(end - now, MAX_HOLD_OFF);
        hold_off_until = end + held;
        TIMER_QUEUE_HELD_PASSES.add(1);
        TIMER_QUEUE_HELD_OFF_NS.add(duration_cast<Nanoseconds>(held).count());
    }

    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        if (num_expired != 0) {
            m_hold_off_until = hold_off_until;
        }

        // The parked timers go back in without a reprogramming each: the one
        // below sees them all, the hold-off included. A timer cancelled while
        // it was parked is no longer in the list.
        while (!m_restart.empty()) {
            auto &timer = m_restart.front().value();
            auto _ = m_restart.pop_front();
            timer.m_parked = false;
            auto const [iter, inserted] = base_of(timer.clockid()).active.insert(timer);
            DEBUG_ASSERT(inserted);
        }
        reprogram_locked();
    }
    notify_refusal();
    return num_expired;
}

} // namespace ours::time
