#include <ours/sync/mutex.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/auto-preempt-disabler.hpp>

#include <ours/time/clock.hpp>

#include <ours/mem/cpu-local.hpp>

#include <arch/halt.hpp>

using ustl::sync::MemoryOrder;
using ours::task::Thread;
using ours::mem::CpuLocal;

namespace ours::sync {
    FORCE_INLINE
    auto Mutex::record_initial_assigned_cpu() -> void {
        m_cpunum.store(Cpu::id(), MemoryOrder::Relaxed);
    }

    FORCE_INLINE
    auto Mutex::reset_initial_assigned_cpu() -> void {
        m_cpunum.store(kInvalidCpuNum, MemoryOrder::Relaxed);
    }

    template <bool TimesliceExtensionEnabled>
    auto Mutex::acquire_common(Duration spining_duration, 
                               TimesliceExtension<TimesliceExtensionEnabled> timeslice_extension) 
    -> bool {
        auto const current_thread = Thread::Current::get();
        auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread)) | State::Contended;

        {
            auto set_timeslice_extension = false;            
            if constexpr (TimesliceExtensionEnabled) {
                current_thread->preemption_state().set_timeslice_extension(timeslice_extension.value);
            }

            /// First attempt to acquire this mutex.
            auto old_mutex_state = State::Available;
            if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, 
                                              MemoryOrder::Acquire, MemoryOrder::Relaxed)) {
                record_initial_assigned_cpu();

                return set_timeslice_extension;
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
        auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread)) | State::Contended;

        task::AutoPreemptDisabler preempt_disabler{task::AutoPreemptDisabler::Defer};
        if constexpr (TimesliceExtensionEnabled) {
            preempt_disabler.disable();
        }

        auto now_ticks = time::SteadyClock::ticks();
        auto const deadline_ticks = now_ticks; // FIXME(SmallHuaZi)

        do {
            auto old_mutex_state = State::Available;
            if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, 
                                              MemoryOrder::Acquire, MemoryOrder::Relaxed)) {
                record_initial_assigned_cpu();

                if constexpr (TimesliceExtensionEnabled) {
                    current_thread->preemption_state().set_timeslice_extension(timeslice_extension.value);
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

            arch::halt();
            now_ticks = time::SteadyClock::ticks();
        } while (now_ticks < deadline_ticks);

        
    }

    auto Mutex::try_release(Thread *current_thread) -> State {
        auto old_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));
        if (m_state.compare_exchange_weak(old_mutex_state, State::Available, 
                                         MemoryOrder::Acquire, MemoryOrder::Relaxed)) {
            return State::Available;
        }

        return old_mutex_state;
    }

    /// Pick new owner and hand off this mutex to it.
    auto Mutex::release_slowpath(Thread *current_thread, State old_mutex_state) -> void {
        Thread::UnparkList to_wake;

        auto &default_hook = OwnedWaitQueue::default_wake_hook();
        if (!m_waiters.lock_for_notify_operation(1, default_hook, to_wake)) {
            // ???
        }

        State new_mutex_state;
        Thread *new_owner;

        auto const do_wake = !to_wake.empty();
        if (do_wake) {
            new_owner = &to_wake.front(); 
            new_mutex_state = static_cast<State>(reinterpret_cast<usize>(new_owner)) | State::Contended;
        } else {
            new_owner = nullptr;
            new_mutex_state = State::Available;
        }

        if (!m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                           MemoryOrder::Acquire, MemoryOrder::Relaxed)) [[unlikely]] {
            panic();
        }

        if (do_wake) {
            m_waiters.notify_threads_locked(ustl::move(to_wake), default_hook, OwnedWaitQueue::NotifyControl::None);
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