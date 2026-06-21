/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_SPIN_LOCK_HPP
#define OURS_SYNC_SPIN_LOCK_HPP 1

#include <ours/types.hpp>
#include <ours/task/types.hpp>

#include <ours/sync/config.hpp>
#include <ours/sync/priv/lock-name-storage.hpp>

#include <lockdep/state-saver.hpp>

#include <arch/interrupt.hpp>
#include <arch/auto-interrupt-disabler.hpp>

#include <ustl/sync/atomic.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::sync {
namespace spinlock {
    enum class Option {
        Monitored = 0_bit,

        NoTrace = 1_bit,

        DisablePreempt = 2_bit,

        DisableIrq = 3_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(Option);

    CXX11_CONSTEXPR
    static auto const kDefaultOptions = Option::DisablePreempt;

    CXX11_CONSTEXPR
    static auto const kMonitoredOptions = Option::Monitored;

    CXX11_CONSTEXPR
    static auto const kNoTraceOptions = Option::NoTrace;

    template <Option Options>
    class TA_CAP("SpinLock") SpinLock
        : public priv::LockNameStorage<SpinLock<Options>, kLockTracingEnabled> {
        using Self = SpinLock;
      public:
        CXX11_CONSTEXPR
        static auto const kIsMonitored = !!(Options & Option::Monitored);

        CXX11_CONSTEXPR
        static auto const kIsNoTrace = !!(Options & Option::NoTrace);

        static auto num_held() -> usize;

        static auto num_held_cpu() -> usize;

        explicit SpinLock();

        ~SpinLock();

        /// Acquire the lock.
        auto acquire() -> void;

        /// Acquire the lock.
        auto acquire(char const *critial_section_name) -> void;

        /// Try to acquire the lock, return true if success.
        auto try_acquire() -> bool;

        /// Release the lock.
        auto release() -> void;

        /// Release the lock.
        auto release(char const *critial_section_name) -> void;

        auto acquire_irq_save(arch::IrqSavedState &state) -> void {
            state = arch::local_irq_save_state();
            acquire();
        }

        auto release_irq_restore(arch::IrqSavedState state) -> void {
            arch::local_irq_restore_state(state);
            release();
        }

        auto is_locked() const -> bool;

        /// Return whether this lock is being held by the current thread.
        auto is_held() const -> bool;

        auto holder() const -> task::Thread *;

        auto assert_held() const -> void;

        USTL_NO_MOVEABLE_AND_COPYABLE(SpinLock);
      private:
        CXX11_CONSTEXPR
        static auto const kIsAutoPreemptDisable = !!(Options & Option::NoTrace);

        auto do_lock() -> bool;

        ustl::sync::AtomicUsize m_lockval;
    };

} // namespace ours::sync::spinlock

    using SpinLock = spinlock::SpinLock<spinlock::kDefaultOptions>;
    using MonitoredSpinLock = spinlock::SpinLock<spinlock::kMonitoredOptions>;
    using NoTraceSpinLock = spinlock::SpinLock<spinlock::kNoTraceOptions>;

    LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::IrqSave, arch::AutoInterruptDisabler);
    LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::PreemptDisable, task::AutoPreemptDisabler);

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SPIN_LOCK_HPP