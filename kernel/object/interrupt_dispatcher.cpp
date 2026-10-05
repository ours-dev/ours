/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/time/timer.hpp>

#include <utility>

namespace ours::object {
InterruptDispatcher::InterruptDispatcher(Flags const flags)
    : Base(), m_event(sync::Event::Options::AutoUnsignal), m_flags(flags) {}

/// Takes what is pending, or parks and looks again once woken. Woken does not
/// mean an interrupt is there to take -- destroy() wakes the waiter too -- so
/// the second look decides, under the lock.
auto InterruptDispatcher::wait(Instant *const out_timestamp) -> Status {
    verify_canary();

    for (;;) {
        if (auto const done = begin_wait(out_timestamp); done.is_some()) {
            return *done;
        }
        if (auto const status = park(); status != Status::Ok) {
            return status;
        }
    }
}

/// Two cases have no branch yet, because neither can arise: no port can be
/// bound (a wait on an interrupt bound to a port is to answer BadState once
/// one can be), and there are no wake vectors.
///
/// Each answer that takes an interrupt unsignals m_event under the lock. The
/// interrupt it takes may have signalled the event with no one parked, and an
/// auto-unsignal event latches such a signal for the next wait. Left standing,
/// it would wake the next park at once, for an interrupt this wait has already
/// taken; that wait would then find NeedAck with nothing pending, move to
/// Waiting and park -- and the time after, find Waiting and answer BadState.
auto InterruptDispatcher::begin_wait(Instant *const out_timestamp) -> ustl::Option<Status> {
    // IrqSave alone: nothing here wakes a thread, so no reschedule can be
    // asked for under the lock. unsignal() only takes the event's queue lock.
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    switch (m_state) {
        case State::Destroyed:
            return Status::Cancelled;

        case State::Triggered:
            m_state = State::NeedAck;
            take_timestamp_locked(out_timestamp);
            m_event.unsignal();
            return Status::Ok;

        case State::NeedAck:
            // This wait is the ack of the interrupt the last one took. One
            // that arrived since is pending, and is taken at once, as if
            // Triggered; the object stays in NeedAck, and the next wait acks
            // this one.
            if (m_timestamp.is_some()) {
                take_timestamp_locked(out_timestamp);
                m_event.unsignal();
                return Status::Ok;
            }
            if (!!(m_flags & Flags::UnmaskPrewait)) {
                unmask_interrupt();
            }
            break;

        case State::Idle:
            break;

        case State::Waiting:
            // Another thread is parked on it: one waiter at a time, since
            // each signal wakes only one.
            return Status::BadState;
    }
    m_state = State::Waiting;
    return ustl::NONE;
}

/// The wait is interruptible, so a kill or a suspend ends it; the object then
/// goes back to Idle if it is still Waiting, so that the next wait does not
/// find it Waiting for a thread that left. If an interrupt arrived meanwhile
/// it is Triggered, and stays so for the next wait to take.
auto InterruptDispatcher::park() -> Status {
    auto const status = m_event.wait(time::Deadline::infinite());
    if (status != Status::Ok) {
        Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
        if (m_state == State::Waiting) {
            m_state = State::Idle;
        }
    }
    return status;
}

auto InterruptDispatcher::take_timestamp_locked(Instant *const out_timestamp) -> void {
    // Every way into Triggered records a time first, and NeedAck reaches here
    // only with one pending.
    DEBUG_ASSERT(m_timestamp.is_some(), "an interrupt taken with no time recorded");
    *out_timestamp = *m_timestamp;
    m_timestamp.reset();
}

/// There is no port to send to yet. A pending time is kept: the first
/// interrupt since the last was taken is the one a wait reports, and later
/// ones fold into it.
///
/// The time is pending until a wait takes it, and it is kept in an Option
/// rather than in a bare time whose 0 means "none pending": with that, a
/// trigger at time 0 would record nothing, and in NeedAck it would be lost
/// outright, so the next wait would park. An Option has no such value.
auto InterruptDispatcher::trigger(Instant const timestamp) -> Status {
    verify_canary();

    if (!(m_flags & Flags::Virtual)) {
        return Status::BadState;
    }

    // Preemption off: the signal may wake a thread, and the reschedule that
    // asks for has to wait until the lock is dropped.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }
    if (m_timestamp.is_none()) {
        m_timestamp = timestamp;
    }

    m_event.signal();

    // In NeedAck the state stays: the time recorded above is what tells the
    // next wait an interrupt is pending, and that wait acks the last one
    // before it takes this one.
    if (m_state != State::NeedAck) {
        m_state = State::Triggered;
    }
    return Status::Ok;
}

/// There is no port yet to take a queued packet back from.
///
/// The line is masked and let go of outside the lock, after the state is set.
/// Once Destroyed is set, a handler that still runs finds it and does nothing.
/// And once physical lines exist, a handler is called under its line's lock
/// (L1), which letting go of the line takes to wait for a handler in flight:
/// holding L2 across that would be L2 -> L1 against the handler's L1 -> L2.
/// Whether this order is right in every interleaving is the design's open
/// question (.agents/tasks/microkernel-interrupts/DESIGN.md, Open questions);
/// for a virtual interrupt the three calls do nothing.
auto InterruptDispatcher::destroy() -> Status {
    verify_canary();

    auto old_state = State::Destroyed;
    {
        Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
        old_state = std::exchange(m_state, State::Destroyed);
    }
    if (old_state == State::Destroyed) {
        return Status::Ok;
    }

    mask_interrupt();
    deactivate_interrupt();
    unregister_interrupt_handler();

    // Wakes a parked waiter, which then finds Destroyed and answers
    // Cancelled; with none parked, the signal is latched and the next wait
    // finds Destroyed before it would park.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    m_event.signal();
    return Status::Ok;
}

auto InterruptDispatcher::on_zero_handles() -> void {
    // Nothing to report to: the last handle is gone, and a destroy that has
    // already run makes this one do nothing.
    auto _ = destroy();
}
} // namespace ours::object
