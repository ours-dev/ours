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
    auto const new_mutex_state =
        static_cast<State>(reinterpret_cast<usize>(current_thread)) | State::Contended;

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
    auto const new_mutex_state =
        static_cast<State>(reinterpret_cast<usize>(current_thread)) | State::Contended;

    task::AutoPreemptDisabler preempt_disabler{task::AutoPreemptDisabler::Defer};
    if constexpr (TimesliceExtensionEnabled) {
        preempt_disabler.disable();
    }

    auto now_ticks = time::SteadyClock::ticks();
    auto const deadline_ticks = now_ticks; // FIXME(SmallHuaZi)

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
            // Stop spinning because the mutex has been contested by other thread.
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
        now_ticks = time::SteadyClock::ticks();
    } while (now_ticks < deadline_ticks);

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
        Guard queue_guard{m_waiters.get_lock()};

        // Once Contended is in m_state it stays there until the owner's
        // release_slowpath() takes it out, under this lock, so a waiter that
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

auto Mutex::try_release(Thread *current_thread) -> State {
    auto old_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));
    if (m_state.compare_exchange_weak(old_mutex_state, State::Available, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
        return State::Available;
    }

    return old_mutex_state;
}

/// Pick new owner and hand off this mutex to it.
auto Mutex::release_slowpath(Thread *current_thread, State old_mutex_state) -> void {
    auto &default_hook = task::OwnedWaitQueue::default_wake_hook();

    // Publishing the new state is the same step either way -- only the value
    // differs -- and it must not be reached twice, so it lives here.
    auto const publish = [&](State new_mutex_state) {
        if (!m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                           std::memory_order_acquire, std::memory_order_relaxed))
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

            // The owner pointer *is* the state: a non-null owner with
            // Contended set is what tells the next releaser there is a queue
            // to consult.
            publish(static_cast<State>(reinterpret_cast<usize>(new_owner)) | State::Contended);
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
