#include <arch/spin_hint.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

using ours::mm::CpuLocal;
using ours::task::Thread;
using ustl::sync::MemoryOrder;

namespace ours::sync {
FORCE_INLINE auto Mutex::record_initial_assigned_cpu() -> void {
    m_cpunum.store(Cpu::id(), MemoryOrder::Relaxed);
}

FORCE_INLINE auto Mutex::reset_initial_assigned_cpu() -> void {
    m_cpunum.store(INVALID_CPU_NUM, MemoryOrder::Relaxed);
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
        if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, MemoryOrder::Acquire,
                                          MemoryOrder::Relaxed)) {
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
        if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, MemoryOrder::Acquire,
                                          MemoryOrder::Relaxed)) {
            record_initial_assigned_cpu();

            if constexpr (TimesliceExtensionEnabled) {
                current_thread->preemption_state().set_timeslice_extension(
                    timeslice_extension.value);
            }
            return false;
        }

        if (!!(old_mutex_state & State::Contended)) {
            // Stop spinning because the mutex has been contested by other thread.
            break;
        }

        auto const curr_cpu = Cpu::id();
        if (curr_cpu == m_cpunum.load(MemoryOrder::Relaxed)) {
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

    // Every exit above either returns false on a successful CAS or breaks
    // here: contended by another thread, self-owned, or the spin deadline
    // passed. None of those acquired the mutex.
    return true;
}

auto Mutex::try_release(Thread *current_thread) -> State {
    auto old_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));
    if (m_state.compare_exchange_weak(old_mutex_state, State::Available, MemoryOrder::Acquire,
                                      MemoryOrder::Relaxed)) {
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
        if (!m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, MemoryOrder::Acquire,
                                           MemoryOrder::Relaxed)) [[unlikely]] {
            panic("Mutex handoff state changed");
        }
    };

    // P0963R3: the queue answers with both halves of one decision -- could it
    // be locked, and who is waiting -- and decomposing that in the condition
    // keeps `to_wake` in scope for the branch that needs it while the
    // contextual conversion asks the only question that matters here.
    //
    // The previous shape could not say this: `lock_for_notify_operation` wrote
    // through an out-parameter and returned a bool nobody acted on, so the
    // successor and the state that publishes it had to be default-initialised
    // above an if/else and a separate `do_wake` carried the answer to the end
    // of the function.
    if (auto [locked, to_wake] = m_waiters.lock_for_notify_operation<Thread::UnparkList>(
            1, default_hook)) {
        auto *const new_owner = &*to_wake.front();

        // The owner pointer *is* the state: a non-null owner with Contended
        // set is what tells the next releaser there is a queue to consult.
        publish(static_cast<State>(reinterpret_cast<usize>(new_owner)) | State::Contended);
        m_waiters.notify_threads_locked(ustl::move(to_wake), default_hook,
                                        task::OwnedWaitQueue::NotifyControl::None);
    } else {
        publish(State::Available);
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
