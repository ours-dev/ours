#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <ustl/option.hpp>

using ours::mm::CpuLocal;
using ours::task::Thread;

namespace ours::sync {
FORCE_INLINE
auto Mutex::record_initial_assigned_cpu() -> void {
    m_cpunum.store(Cpu::id(), std::memory_order_relaxed);
}

FORCE_INLINE
auto Mutex::reset_initial_assigned_cpu() -> void {
    m_cpunum.store(INVALID_CPU_NUM, std::memory_order_relaxed);
}

template <bool TimesliceExtensionEnabled>
auto Mutex::acquire_common(Duration spining_duration,
                           TimesliceExtension<TimesliceExtensionEnabled> timeslice_extension)
    -> bool {
    auto const current_thread = Thread::Current::get();
    // The bare thread pointer, with no flag bits. Contended means "a waiter
    // may be queued" and is written only under the wait-queue lock, by a
    // contender on its way to park (acquire_slowpath) or by the releaser
    // handing off (release_slowpath). An uncontended acquire leaves it
    // clear, which is what lets try_release() take the fast path back out.
    //
    // This used to OR Contended in on every acquire. try_release() expects
    // the bare pointer, so its CAS failed every time and every release took
    // the queue lock; and a spinner in acquire_slowpath() saw Contended on
    // an uncontended mutex and gave up spinning at once. Zircon's
    // Mutex::AcquireCommon() stores the bare pointer too.
    auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));

    {
        if constexpr (TimesliceExtensionEnabled) {
            current_thread->preemption_state().set_timeslice_extension(timeslice_extension.value);
        }

        /// First attempt to acquire this mutex.
        auto old_mutex_state = State::Available;
        if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                          std::memory_order_acquire, std::memory_order_relaxed)) {
            record_initial_assigned_cpu();

            return false;
        }

        if constexpr (TimesliceExtensionEnabled) {
            current_thread->preemption_state().cancel_timeslice_extension();
        }
    }

    return acquire_slowpath(spining_duration, current_thread, timeslice_extension);
}

template <bool TimesliceExtensionEnabled>
auto Mutex::acquire_slowpath(Duration spining_duration, task::Thread *current_thread,
                             TimesliceExtension<TimesliceExtensionEnabled> timeslice_extension)
    -> bool {
    // Bare, for the same reason as in acquire_common(): whoever takes the
    // mutex here takes it without a queue behind it.
    auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));

    task::AutoPreemptDisabler preempt_disabler{task::AutoPreemptDisabler::Defer};
    if constexpr (TimesliceExtensionEnabled) {
        preempt_disabler.disable();
    }

    // The adaptive spin: keep retrying for |spining_duration| on the bet that
    // the owner is running on another CPU and about to let go, which is far
    // cheaper than parking and being woken. The loop below cuts the bet off
    // early when it is plainly lost -- somebody already queued, or the owner
    // last seen on this very CPU.
    //
    // The deadline used to be `now_ticks` itself, with a FIXME, so the loop
    // body ran exactly once and every contended acquire went straight to the
    // park. It is taken on the steady clock's time rather than in raw ticks:
    // there is no duration-to-ticks conversion to build it with, and a tick
    // count means nothing across a clock-source switch. Zircon converts once
    // through timer_get_ticks_to_time_ratio(); reading now() per iteration
    // costs a multiply, which is noise next to a spin hint.
    //
    // Before the first clock source enrolls, the steady clock reads zero and
    // stays there (Clock::time_of), so a deadline on it would never arrive
    // and the loop would spin for as long as the owner held on. Skip the spin
    // then: one attempt, then the park, which is what every acquire did
    // before the deadline was fixed.
    auto const spin_start = time::SteadyClock::now();
    auto spin_until = spin_start;
    if (spin_start.time_since_epoch() != Duration::zero()) {
        spin_until += spining_duration;
    }

    do {
        auto old_mutex_state = State::Available;
        if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                          std::memory_order_acquire, std::memory_order_relaxed)) {
            record_initial_assigned_cpu();

            if constexpr (TimesliceExtensionEnabled) {
                return current_thread->preemption_state().set_timeslice_extension(
                    timeslice_extension.value);
            }
            return false;
        }

        if (!!(old_mutex_state & State::Contended)) {
            // Somebody has already given up spinning and queued (or is about
            // to, under the queue lock). The owner will hand the mutex to the
            // head of that queue, not to whoever spins, so spinning further
            // only burns this CPU. Zircon: "All spinners convert to blocking
            // when the first one reaches the max spin duration."
            break;
        }

        auto const curr_cpu = Cpu::id();
        if (curr_cpu == m_cpunum.load(std::memory_order_relaxed)) {
            break;
        }

        if constexpr (TimesliceExtensionEnabled) {
            if (current_thread->preemption_state().has_preempt_request()) {
                preempt_disabler.enable();
                // Give current CPU a chance to reschedule.
                preempt_disabler.disable();
            }
        }

        arch::spin_hint();
    } while (time::SteadyClock::now() < spin_until);

    // Every exit above either returned false on a successful CAS or broke out
    // to here: contended by another thread, self-owned, or the spin deadline
    // passed. None of those acquired the mutex. What happens next -- take it
    // after all, or queue for it -- is decided under the wait queue's lock,
    // the one release_slowpath() holds while it chooses between handing off
    // and publishing Available.
    //
    // It used to be decided outside that lock: the failed CAS above was the
    // last look at m_state, and park_and_assign_owner() then enqueued under a
    // lock it took by itself. A release landing between the two found the
    // queue empty and published Available; this thread then queued on a mutex
    // nobody held and nobody would release, and slept for good.
    //
    // The PI lock goes outside the queue lock, the order the tree uses (see
    // WaitQueue::enqueue_waiter), and with interrupts and preemption off
    // because the postamble carries it into reschedule_common(). Same shape
    // as Event::wait_etc().
    Guard<SpinLock, IrqSaveAndPreemptDisable> pi_guard{current_thread->get_pi_lock()};
    {
        // IrqSave although pi_guard already has interrupts off: a wait
        // queue's lock is also taken from interrupt context -- a timeout
        // firing wakes its waiter through it -- so every holder states that
        // it holds it with interrupts off, rather than inheriting that from
        // whatever happens to surround it.
        Guard<SpinLock, IrqSave> queue_guard{m_waiters.get_lock()};

        // Contended is written only under this lock -- set by contenders here,
        // taken out by the owner's release_slowpath() -- so a waiter that
        // finds it set has nothing to write. Otherwise there are two cases,
        // and the loop only re-runs when a CAS loses to a fast-path acquire
        // or release that changed m_state under it:
        //
        //  - Available: the owner released after the CAS above failed.
        //    Nobody can be queued behind that (a queued waiter implies
        //    Contended), so take the mutex rather than park waiting for a
        //    release that already happened.
        //  - Held: set Contended on it. That is what sends the owner's
        //    release down the slow path, where it will find this thread
        //    queued.
        //
        // Zircon's AcquireContendedMutex() does the same with one fetch_or,
        // storing the owner afterwards if the old value was free; State is an
        // enum, which std::atomic gives no fetch_or, and the CAS also never
        // shows other CPUs an ownerless Contended in between.
        auto old_mutex_state = m_state.load(std::memory_order_relaxed);
        while (!(old_mutex_state & State::Contended)) {
            if (State::Available == old_mutex_state) {
                if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    record_initial_assigned_cpu();

                    if constexpr (TimesliceExtensionEnabled) {
                        return current_thread->preemption_state().set_timeslice_extension(
                            timeslice_extension.value);
                    }
                    return false;
                }
            } else if (m_state.compare_exchange_weak(
                           old_mutex_state, old_mutex_state | State::Contended,
                           std::memory_order_acq_rel, std::memory_order_relaxed)) {
                break;
            }
        }

        // Held by somebody, with Contended in m_state: whenever that owner
        // releases, it has to take this lock first and will find this thread
        // queued.
        auto const status = m_waiters.park_and_assign_owner_preamble_locked(
            current_thread, time::Deadline::infinite(), task::Interruptible::No);
        if (Status::Ok != status) {
            // Could not park -- the caller retries rather than proceeding
            // unlocked, which is the whole point of reporting "not acquired".
            return true;
        }
    }

    auto const status =
        m_waiters.park_and_assign_owner_postamble(current_thread, time::Deadline::infinite());
    if (Status::Ok != status) {
        return true;
    }

    // Held on return, and not by racing for it again: release_slowpath()
    // publishes the woken thread as the new owner *before* notifying it
    // (see the publish() call there), so this is a hand-off. Re-testing
    // m_state here would be a second acquisition attempt against a mutex
    // this thread already owns.
    return false;
}

/// The uncontended release: the bare owner pointer back to Available.
///
/// Strong, not weak. A spurious failure would send the caller into
/// release_slowpath() with the bare pointer as the "observed" state, and the
/// publish there -- a CAS expecting exactly that value -- would panic the
/// moment a contender had set Contended in the meantime.
///
/// Release on success, so the critical section's stores are visible to
/// whoever acquires next; acquire on failure, so the slow path starts from
/// the state the contender published. The old orders were acquire/relaxed,
/// which let the critical section leak past the unlock on any machine
/// weaker than x86. Same pair as Zircon's Mutex::TryRelease().
auto Mutex::try_release(Thread *current_thread) -> State {
    auto old_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));
    if (m_state.compare_exchange_strong(old_mutex_state, State::Available,
                                        std::memory_order_release, std::memory_order_acquire)) {
        return State::Available;
    }

    return old_mutex_state;
}

/// Pick new owner and hand off this mutex to it.
auto Mutex::release_slowpath(Thread *current_thread, State old_mutex_state) -> void {
    auto &default_hook = task::OwnedWaitQueue::default_wake_hook();

    // Publishing the new state is the same step either way -- only the value
    // differs -- and it must not be reached twice, so it lives here.
    //
    // The CAS is a consistency check as much as a store: with the queue lock
    // held, nothing but this owner may change a Contended state, so a
    // mismatch is corruption, not a race to retry. It has to be strong for
    // that to hold -- a spurious weak failure would panic a healthy mutex --
    // and release-ordered for the same reason try_release() is.
    auto const publish = [&](State new_mutex_state) {
        if (!m_state.compare_exchange_strong(old_mutex_state, new_mutex_state,
                                             std::memory_order_release, std::memory_order_relaxed))
            [[unlikely]] {
            panic("Mutex handoff state changed");
        }
    };

    // Preemption stays off from the publish to the wake. Between the two the
    // mutex is owned by a thread that is not running yet; were this thread
    // preempted there, the mutex would sit with a sleeping owner for as long
    // as the preemption lasted and every other contender would queue behind
    // it. Zircon's ReleaseContendedMutex() holds preemption off across the
    // same span, for the same reason.
    task::AutoPreemptDisabler const preempt_disabler;

    // The option is engaged only when there is somebody to hand off to:
    // locking an empty queue succeeds and still leaves nothing to wake.
    ustl::Option<Thread::UnparkList> batch;
    {
        // Dequeue and publish under the queue lock, as one step. The lock is
        // what a waiter has to take to join the queue, so holding it here
        // means "nobody is queued" stays true until Available is in m_state.
        // Interrupts off because this is a spin lock the timeout handler can
        // take from interrupt context (WaitQueue::handle_timeout); nothing
        // parks on a mutex with a deadline today, but the queue does not know
        // that.
        Guard<SpinLock, IrqSave> guard{m_waiters.get_lock()};
        batch = m_waiters.lock_for_notify_operation<Thread::UnparkList>(1, default_hook);

        if (batch) {
            auto *const new_owner = &*batch->front();

            // The owner pointer *is* the state, and Contended goes with it only
            // while somebody is still queued -- it is what tells the new
            // owner's release there is a queue to consult. Handing off to the
            // last waiter leaves it clear, so that release takes the fast path
            // like any other uncontended one. Zircon's ReleaseContendedMutex()
            // sets STATE_FLAG_CONTESTED on the same condition.
            auto new_mutex_state = static_cast<State>(reinterpret_cast<usize>(new_owner));
            if (!m_waiters.is_empty()) {
                new_mutex_state |= State::Contended;
            }
            publish(new_mutex_state);
        } else {
            publish(State::Available);
        }
    }

    // The wake runs with the queue lock dropped: it takes the new owner's PI
    // lock, and that lock is ordered outside the queue lock. See
    // OwnedWaitQueue::notify_threads(). Nothing after the wake may touch the
    // mutex -- once running, its new owner is free to release and free it.
    if (batch) {
        m_waiters.notify_threads(ustl::move(*batch), default_hook,
                                 task::OwnedWaitQueue::NotifyOptions::None);
    }
}

auto Mutex::release() -> void {
    m_canary.verify<Self>();

    reset_initial_assigned_cpu();

    auto const current_thread = Thread::Current::get();
    if (auto const old_mutex_state = try_release(current_thread);
        old_mutex_state != State::Available) {
        release_slowpath(current_thread, old_mutex_state);
    }
}

template auto Mutex::acquire_common(Duration, TimesliceExtension<false>) -> bool;
template auto Mutex::acquire_common(Duration, TimesliceExtension<true>) -> bool;

} // namespace ours::sync
