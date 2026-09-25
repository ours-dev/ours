#include <ours/lockdep.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>

#include <arch/auto_interrupt_disabler.hpp>

namespace ours::sync {
using task::Scheduler;
using task::Thread;

/// Blocks until the event is signalled -- or returns at once if it already is.
///
/// The test used to be the wrong way round: `m_result.is_some()` means the
/// event *has* been signalled, and that was the branch that parked, while an
/// unsignalled event returned Status::Ok immediately. Every waiter therefore
/// did the opposite of what it asked for, and a caller that loops on wait()
/// -- DpcRunner::Queue::do_work() does -- became a busy loop that ate the CPU
/// and starved every other thread on it.
///
/// An auto-unsignal event consumes the signal here, so that only one waiter
/// is released per signal; a manual-reset one leaves it standing until
/// unsignal(). This mirrors Zircon's Event::WaitWorker(), which returns
/// result_ directly when it is not kNotSignalled, clearing it first for
/// AUTOUNSIGNAL.
auto Event::wait_etc(time::Deadline deadline, task::Interruptible interruptible,
                     task::ThreadSignals sigmask) -> Status {
    // Interrupts and preemption both off, not just the lock: this guard is
    // carried into reschedule_common() through park_thread(), and that is the
    // state it requires and asserts. A plain Guard left interrupts on and the
    // first park died on "Enter reschedule_common() with IRQ enabled". Same
    // shape as Thread::Current::sleep_etc(), and as Zircon's parks, which run
    // under PreemptDisableAndIrqSave.
    auto const current_thread = Thread::Current::get();
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};

    // m_result belongs to the wait queue's lock, not the PI lock: signal()
    // and unsignal() both write it under the queue lock, so reading it under
    // anything else races them. The scope ends before park_thread() because
    // park_thread() takes the same lock on its way to enqueue_waiter(), and
    // SpinLock does not nest. Taking the queue lock underneath the PI lock is
    // the direction the rest of the tree already uses -- see the note on
    // WaitQueue::enqueue_waiter.
    //
    // And the queue lock is held from the check through the enqueue, not
    // dropped in between as it used to be. In that gap a signal() on another
    // CPU found the result unset and the queue empty, latched the result and
    // woke nobody; this thread then enqueued and slept for good -- and every
    // later signal() returned early on the latched result. Holding the lock
    // across both means a signal either lands before the check (and the check
    // sees it) or after the enqueue (and finds this thread to wake). See
    // WaitQueue::park_thread_preamble_locked().
    {
        // IrqSave although the PI guard above already has interrupts off; see
        // the same guard in Mutex::acquire_slowpath().
        Guard<SpinLock, IrqSave> queue_guard{m_wq.get_lock()};
        if (m_result.is_some()) {
            auto const result = *m_result;
            if (!!(m_flags & Flags::AutoUnsignal)) {
                m_result.reset();
            }
            return result;
        }

        auto const status =
            m_wq.park_thread_preamble_locked(current_thread, deadline, sigmask, interruptible);
        if (Status::Ok != status) {
            return status;
        }
    }

    return m_wq.park_thread_postamble(current_thread, deadline);
}

/// Puts the event back into the unsignalled state, so a later wait()
/// blocks again. Takes the wait queue lock because signal() decides what
/// to do by reading m_result under it.
///
/// Interrupts off, like every holder of a wait queue's lock: a timeout that
/// fires on this CPU wakes its waiter through the same lock, and would spin
/// on it for ever underneath a holder it interrupted. This one used to take
/// the lock bare -- `Guard guard{...}` is Guard<SpinLock> with no option,
/// which saves no state at all.
auto Event::unsignal() -> void {
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};
    m_result.reset();
}

/// Signals the event, waking waiters and -- unless exactly one waiter
/// consumed it -- latching the result so a later wait() returns at once.
///
/// The latch is the part that used to be missing. signal() returned early
/// when the queue was empty, before m_result was ever written, so a signal
/// delivered before its waiter arrived was dropped and that waiter then slept
/// forever. The rule that decides it was already written below and simply
/// unreachable: latch for a manual-reset event always, and for an
/// auto-unsignal event only when there was nobody to hand the signal to.
/// That is Zircon's Event::SignalEtc(), where result_ is assigned for a
/// non-AUTOUNSIGNAL event regardless of waiters and only an AUTOUNSIGNAL
/// event may consume the signal outright.
///
/// The empty-queue case is not a special case any more: lock_for_notify_one/
/// all already answer none() for an empty queue, which is the same "nobody
/// took it" the latch rule asks about.
auto Event::signal(Status status) -> void {
    // Interrupts off for all of it: the queue lock here, and the PI and
    // run-queue locks Scheduler::unpark_thread() takes below, are spin locks
    // an interrupt on this CPU can want too -- the tick takes the run-queue
    // lock. With interrupts on, a tick that lands while one of them is held
    // spins on it for ever. This was safe only while every caller happened to
    // run with interrupts off; object::wait_one's observer signals from system
    // call context, which runs with them on. Zircon's Event::SignalEtc() holds
    // its lock IrqSave for the same reason.
    //
    // The disabler is kept even though the guard below is IrqSave as well,
    // because it has to outlast the guard's unlock(): unpark_thread() runs
    // after the queue lock is dropped and still needs interrupts off. Today
    // a Guard's savers happen to live until the guard is destroyed, not
    // until unlock(), but that is not a promise -- Zircon's Guard::Release()
    // restores the saved interrupt state along with the lock, and
    // lockdep/docs/interface.md moves this Guard to that shape.
    arch::AutoInterruptDisabler const irq_disabler;
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};

    // Already signalled and not yet consumed. A second signal has nothing to
    // add: the standing result is what the next waiter will read, and no one
    // can be parked, because wait_etc() returns without parking while the
    // result stands.
    if (m_result.is_some()) {
        return;
    }

    ustl::Option<Thread::UnparkList> unpark_threads;
    if (!!(m_flags & Flags::AutoUnsignal)) {
        unpark_threads = m_wq.lock_for_notify_one<Thread::UnparkList>(status);
    } else {
        unpark_threads = m_wq.lock_for_notify_all<Thread::UnparkList>(status);
    }

    auto const need_unpark = unpark_threads && !unpark_threads->empty();
    if (!(m_flags & Flags::AutoUnsignal) || !need_unpark) {
        m_result = status;
    }

    if (need_unpark) {
        guard.unlock();
        Scheduler::unpark_thread(ustl::move(*unpark_threads));
    }
}
} // namespace ours::sync
