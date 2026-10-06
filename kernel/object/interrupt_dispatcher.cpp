/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/lockdep.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/panic.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <atomic>
#include <utility>

namespace ours::object {
namespace {
    /// wait()'s hook between begin_wait() and park(): see
    /// set_before_park_hook_for_test(). Atomic, because a test installs it
    /// on one CPU while waits run on others; release and acquire, so that a
    /// waiter that finds the hook also finds what the test set up for it
    /// before installing it.
    // Namespace-scope objects are UPPER_CASE throughout this tree.
    // NOLINTNEXTLINE(readability-identifier-naming)
    constinit std::atomic<InterruptDispatcher::BeforeParkHook> BEFORE_PARK_HOOK{nullptr};
} // namespace

auto InterruptDispatcher::set_before_park_hook_for_test(BeforeParkHook const hook) -> void {
    BEFORE_PARK_HOOK.store(hook, std::memory_order_release);
}

InterruptDispatcher::InterruptDispatcher(Flags const flags)
    : Base(), m_event(sync::Event::Options::AutoUnsignal), m_flags(flags) {}

/// The object's action is on its line already, and the line's handler
/// reads the flags under the lock in interrupt context, so they are written
/// under it too. The line is masked until the create that calls this unmasks
/// it, so the only interrupts that can reach the object before this were
/// already on their way before the mask, or were raised by hand on the
/// line's vector. Either is delivered as on an edge line, without the mask,
/// which leaves nothing wrong behind: the line is masked anyway, and the
/// first interrupt after the unmask is delivered with the mask.
auto InterruptDispatcher::set_level_triggered() -> void {
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    m_flags |= Flags::MaskPostwait | Flags::UnmaskPrewait;
}

/// Takes what is pending, or parks and looks again once woken. Woken does not
/// mean an interrupt is there to take -- destroy() wakes the waiter too -- so
/// the second look decides, under the lock.
auto InterruptDispatcher::wait(Instant *const out_timestamp) -> Status {
    verify_canary();

    for (;;) {
        if (auto const done = begin_wait(out_timestamp); done.is_some()) {
            return *done;
        }
        if (auto const hook = BEFORE_PARK_HOOK.load(std::memory_order_acquire); hook != nullptr) {
            hook(*this);
        }
        if (auto const status = park(); status != Status::Ok) {
            return status;
        }
    }
}

/// A bound interrupt answers BadState before its state is looked at: its
/// interrupts go to the port, and a wait would take one the port should
/// have had. That holds once it is destroyed too, since destroy() keeps the
/// binding.
///
/// One waiter at a time, and m_waiter, not the state, says whether there is
/// one. A thread whose wait found nothing to take is m_waiter from then until
/// its park returns, and every other wait meanwhile answers BadState, whatever
/// the state says. The state cannot say it: a trigger moves Waiting on to
/// Triggered before the waiter has parked. Read from the state, a second wait
/// then found Triggered, took the interrupt and unsignalled the signal the
/// trigger had latched for the first waiter; the first parked with nothing to
/// wake it, the second's next wait parked beside it, and a destroy, which
/// signals once, woke only one of the two.
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
    if (m_port) {
        return Status::BadState;
    }
    // Before the waiter: a wait on a destroyed object is told so, even while
    // the waiter the destroy woke has not left yet.
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }
    if (m_waiter != nullptr) {
        return Status::BadState;
    }
    switch (m_state) {
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
        case State::Destroyed:
            // Neither gets here: Waiting stands only while m_waiter is set,
            // since this sets the two together and park() clears them
            // together, and Destroyed is answered above.
            unreachable();
    }
    m_state = State::Waiting;
    m_waiter = task::Thread::Current::get();
    return ustl::NONE;
}

/// The waiter gives up m_waiter as its park returns, however the park ended,
/// and Waiting goes with it, so the next wait neither finds Waiting with no
/// one there nor is refused for a thread that has left. Woken by a trigger or
/// a destroy, the state has moved on already. Ended early -- the wait is
/// interruptible, so a kill or a suspend ends it -- the object goes back to
/// Idle; if an interrupt arrived meanwhile it is Triggered, and stays so for
/// the next wait to take.
///
/// From then on another wait may come in. Woken, the waiter goes round wait()
/// again and looks under the lock for what woke it; a wait that gets the lock
/// first may take that interrupt, and the woken one then parks again. Either
/// way one thread at most is parked.
auto InterruptDispatcher::park() -> Status {
    auto const status = m_event.wait(time::Deadline::infinite());
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    DEBUG_ASSERT(m_waiter == task::Thread::Current::get(),
                 "a thread parked on an interrupt it was not the waiter of");
    m_waiter = nullptr;
    if (m_state == State::Waiting) {
        m_state = State::Idle;
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

/// A pending time is kept: the first interrupt since the last was taken is
/// the one a wait or a packet reports, and later ones fold into it.
///
/// The time is pending until a wait takes it, and it is kept in an Option
/// rather than in a bare time whose 0 means "none pending": with that, a
/// trigger at time 0 would record nothing, and in NeedAck it would be lost
/// outright, so the next wait would park. An Option has no such value.
auto InterruptDispatcher::trigger(Instant const timestamp) -> Status {
    verify_canary();

    // Preemption off: the signal may wake a thread, and the reschedule that
    // asks for has to wait until the lock is dropped.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    // Under the lock, as every read of the flags is, though Virtual itself
    // never changes: a line's object adds its level bits once it is on the
    // line (set_level_triggered()).
    if (!(m_flags & Flags::Virtual)) {
        return Status::BadState;
    }
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }
    if (m_timestamp.is_none()) {
        m_timestamp = timestamp;
    }
    deliver_locked();
    return Status::Ok;
}

/// A line's interrupt brings no time of its own, so it is taken here, on the
/// clock interrupt_wait's and the packets' times are read against.
///
/// The guard turns interrupts and preemption off although interrupt context
/// has both off already. It costs a nesting count, and keeps the path right
/// when a kernel test drives a dispatch from a thread: a signal or a post
/// below may wake a thread, and the reschedule that asks for must wait until
/// the lock is dropped, as in trigger().
///
/// Destroyed is checked under the lock, which is what lets destroy() let the
/// line go without holding it: a handler that got here after destroy() set
/// the state delivers nothing, and destroy()'s detach waits for it to return.
auto InterruptDispatcher::handle_interrupt() -> void {
    verify_canary();

    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (m_state == State::Destroyed) {
        return;
    }
    if (m_timestamp.is_none()) {
        m_timestamp = time::SteadyClock::now().time_since_epoch().count();
    }
    deliver_locked();
}

auto InterruptDispatcher::deliver_locked() -> void {
    // Bound: a packet goes out unless one is waiting for its ack, in which
    // case the time recorded is what ack() sends next.
    if (m_port) {
        if (m_state != State::NeedAck) {
            auto _ = send_packet_locked(); // A packet still queued takes this one in.
            m_state = State::NeedAck;
        }
        return;
    }

    // Unbound, a level line is masked before its waiter is woken, and the
    // wait after the one this wakes unmasks it, as that wait is the ack
    // (UnmaskPrewait). Bound, send_packet_locked() masks it instead.
    if (!!(m_flags & Flags::MaskPostwait)) {
        mask_interrupt();
    }
    m_event.signal();

    // In NeedAck the state stays: the time recorded is what tells the next
    // wait an interrupt is pending, and that wait acks the last one before
    // it takes this one.
    if (m_state != State::NeedAck) {
        m_state = State::Triggered;
    }
}

/// Queues under the port's interrupt lock (L3), taken inside m_spinlock
/// (L2), and the port posts its semaphore after letting L3 go. The waiter
/// that post wakes may take the packet on another CPU while this one still
/// holds L2; it reads the packet only under L3, so L2 need not cover it.
///
/// The time is cleared whether or not the packet went on the queue. If it
/// was still queued, unread, the reader has not seen the last interrupt yet
/// and gets this one with it: one packet stands for every interrupt up to
/// the moment it is read.
auto InterruptDispatcher::send_packet_locked() -> bool {
    DEBUG_ASSERT(m_timestamp.is_some(), "a packet sent with no time recorded");
    auto const queued = m_port->queue_interrupt_packet(&m_port_packet, *m_timestamp);
    if (!!(m_flags & Flags::MaskPostwait)) {
        mask_interrupt();
    }
    m_timestamp.reset();
    return queued;
}

/// The line is masked and let go of outside the lock, after the state is set.
/// Once Destroyed is set, a handler that still runs finds it and does nothing.
/// A physical line's handler is called under its line's lock (L1), which
/// letting go of the line takes to wait for a handler in flight: holding L2
/// across that would be L2 -> L1 against the handler's L1 -> L2. Whether this
/// order is right in every interleaving is the design's open question
/// (.agents/tasks/microkernel-interrupts/DESIGN.md, Open questions); for a
/// virtual interrupt the three calls do nothing.
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

    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};

    // Bound: no thread is parked to wake, since a bind refuses an interrupt
    // a thread waits on (m_waiter) and a wait refuses a bound one. The packet
    // comes off the port instead, as the port keeps only a pointer into this
    // object. Once Destroyed is set nothing queues it again, so after this
    // no port holds it.
    if (m_port) {
        auto const removed = m_port->remove_interrupt_packet(&m_port_packet);
        if (old_state == State::NeedAck && !removed) {
            return Status::NotFound;
        }
        return Status::Ok;
    }

    // Wakes the parked waiter -- one at most, as m_waiter keeps it -- which
    // then finds Destroyed and answers Cancelled; with none parked, the
    // signal is latched and the next wait finds Destroyed before it would
    // park.
    m_event.signal();
    return Status::Ok;
}

auto InterruptDispatcher::bind(ustl::Arc<PortDispatcher> port, u64 const key) -> Status {
    verify_canary();

    // Preemption off: a packet sent here posts the port's semaphore, which
    // may wake a thread.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }
    // Nothing signals m_event once bound, nor does a destroy of a bound
    // interrupt, so a thread left waiting under a bind would stay parked for
    // good. m_waiter, not Waiting, says whether one is: a trigger moves the
    // state on to Triggered before the waiter has parked, and a bind then
    // took the interrupt to the port and unsignalled the waiter's wakeup.
    if (m_waiter != nullptr) {
        return Status::BadState;
    }
    if (m_port) {
        return Status::AlreadyBound;
    }

    // The packet is on no port's queue: an unbind took it back or found it
    // read, and a never-bound interrupt never sent it. So the key can be
    // written under L2 alone; the port reads it under L3 once queued.
    m_port = ustl::move(port);
    m_port_packet.key = key;

    // Nothing signals m_event while bound, and nothing may stand latched on
    // it either. A trigger before the bind, with no one parked, latched a
    // signal for an interrupt the port gets from now on: left standing, it
    // would wake the first wait after an unbind at once, and that wait would
    // find the Waiting it set itself and answer BadState.
    m_event.unsignal();

    // An interrupt nobody has taken yet goes out now. One in NeedAck waits
    // for the ack: the wait that took the last interrupt is still owed one.
    if (m_state == State::Triggered) {
        auto _ = send_packet_locked(); // Never queued here, so it cannot fold.
        m_state = State::NeedAck;
    }
    return Status::Ok;
}

/// The port reference is moved out under the lock and dropped after it:
/// the last reference to a port runs its destructor, which is no work for a
/// spin lock held with interrupts off.
auto InterruptDispatcher::unbind(ustl::Arc<PortDispatcher> const &port) -> Status {
    verify_canary();

    ustl::Arc<PortDispatcher> unbound;
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    if (m_port != port) {
        return Status::NotFound;
    }
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }

    // Taken back first, while the port still holds it, so that no packet of
    // an unbound interrupt stays queued: a port_wait after the unbind would
    // otherwise read a packet whose interrupt may already be gone.
    auto const removed = m_port->remove_interrupt_packet(&m_port_packet);
    m_port_packet.key = 0;
    unbound = ustl::move(m_port);

    // A packet nobody read was never delivered, so the interrupt it carried
    // is pending again, for a wait or the next bind. A packet already read
    // was delivered, and the interrupt stays in NeedAck, owed its ack -- now
    // a wait, as it is unbound.
    if (m_state == State::NeedAck && removed) {
        m_state = State::Triggered;
        m_timestamp = m_port_packet.timestamp;
        m_port_packet.timestamp = 0;
    }
    return Status::Ok;
}

auto InterruptDispatcher::ack() -> Status {
    verify_canary();

    // Preemption off: the next packet, if one goes out, may wake a thread.
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (!m_port) {
        return Status::BadState;
    }
    if (m_state == State::Destroyed) {
        return Status::Cancelled;
    }
    if (m_state != State::NeedAck) {
        return Status::Ok;
    }

    if (!!(m_flags & Flags::UnmaskPrewait)) {
        unmask_interrupt();
    }

    // Interrupts arrived since the last packet: the next goes out at once,
    // and the object stays in NeedAck, owed an ack for it.
    if (m_timestamp.is_some()) {
        if (!send_packet_locked()) {
            // The last packet is still queued, unread, and the ack came
            // before the reader took it. The interrupts since fold into it.
            return Status::BadState;
        }
        return Status::Ok;
    }
    m_state = State::Idle;
    return Status::Ok;
}

auto InterruptDispatcher::on_zero_handles() -> void {
    // Nothing to report to: the last handle is gone, and a destroy that has
    // already run makes this one do nothing.
    auto _ = destroy();
}
} // namespace ours::object
