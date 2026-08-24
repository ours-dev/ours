#include <ours/sync/mutex.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/auto-preempt-disabler.hpp>

#include <ours/time/clock.hpp>

#include <ours/mm/cpu-local.hpp>

#include <arch/halt.hpp>

using ustl::sync::MemoryOrder;
using ours::task::Thread;
using ours::mm::CpuLocal;

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
        auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));

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
        auto const new_mutex_state = static_cast<State>(reinterpret_cast<usize>(current_thread));

        task::AutoPreemptDisabler preempt_disabler{task::AutoPreemptDisabler::Defer};
        if constexpr (TimesliceExtensionEnabled) {
            preempt_disabler.disable();
        }

        for (;;) {
            auto old_mutex_state = State::Available;
            if (m_state.compare_exchange_weak(old_mutex_state, new_mutex_state, 
                                              MemoryOrder::Acquire, MemoryOrder::Relaxed)) {
                record_initial_assigned_cpu();

                if constexpr (TimesliceExtensionEnabled) {
                    current_thread->preemption_state().set_timeslice_extension(timeslice_extension.value);
                }
                return false;
            }

            if (!(old_mutex_state & State::Contended)) {
                auto const contended_state = old_mutex_state | State::Contended;
                static_cast<void>(m_state.compare_exchange_weak(
                    old_mutex_state, contended_state, MemoryOrder::Acquire, MemoryOrder::Relaxed));
            }

            if constexpr (TimesliceExtensionEnabled) {
                if (current_thread->preemption_state().has_preempt_request()) {
                    preempt_disabler.enable();
                    // Give current CPU a chance to reschedule.
                    preempt_disabler.disable();
                }
            }

#if defined(ARCH_X86)
            asm volatile("pause" ::: "memory");
#else
            asm volatile("" ::: "memory");
#endif
        }
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
            new_owner = &*to_wake.front();
            new_mutex_state = static_cast<State>(reinterpret_cast<usize>(new_owner)) | State::Contended;
        } else {
            new_owner = nullptr;
            new_mutex_state = State::Available;
        }

        if (!m_state.compare_exchange_weak(old_mutex_state, new_mutex_state,
                                           MemoryOrder::Acquire, MemoryOrder::Relaxed)) [[unlikely]] {
            panic("Mutex handoff state changed");
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
