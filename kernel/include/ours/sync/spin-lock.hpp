/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_SPIN_LOCK_HPP
#define OURS_SYNC_SPIN_LOCK_HPP 1

#include <arch/auto-interrupt-disabler.hpp>
#include <arch/interrupt.hpp>
#include <lockdep/state-saver.hpp>
#include <ours/sync/config.hpp>
#include <ours/sync/priv/lock-name-storage.hpp>
#include <ours/task/types.hpp>
#include <ours/types.hpp>
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

    /// Every one of these disables preemption. A spin lock whose holder can
    /// be preempted is a deadlock waiting to happen: the thread scheduled in
    /// its place spins forever on the same CPU, and every other CPU spins
    /// until the holder is scheduled back. Monitored and NoTrace only select
    /// how much bookkeeping the lock does, never whether it is safe to hold.
    constexpr static auto const kDefaultOptions = Option::DisablePreempt;

    constexpr static auto const kMonitoredOptions = Option::Monitored | Option::DisablePreempt;

    constexpr static auto const kNoTraceOptions = Option::NoTrace | Option::DisablePreempt;

    template <Option Options>
    class TA_CAP("SpinLock") SpinLock
        : public priv::LockNameStorage<SpinLock<Options>, kLockTracingEnabled> {
        using Self = SpinLock;

      public:
        constexpr static auto const kIsMonitored = !!(Options & Option::Monitored);

        constexpr static auto const kIsNoTrace = !!(Options & Option::NoTrace);

        static auto num_held() -> usize;

        static auto num_held_cpu() -> usize;

        explicit SpinLock();

        ~SpinLock();

        /// Acquire the lock.
        auto acquire() -> void;

        /// Try to acquire the lock, return true if success.
        auto try_acquire() -> bool;

        /// Release the lock.
        auto release() -> void;

        auto acquire_irq_save(arch::IrqSavedState &state) -> void {
            state = arch::local_irq_save_state();
            acquire();
        }

        /// Order matters: the lock has to be dropped before interrupts come
        /// back. Restoring first leaves a window in which an interrupt taken
        /// on this CPU can reach for this same lock and self-deadlock.
        auto release_irq_restore(arch::IrqSavedState state) -> void {
            release();
            arch::local_irq_restore_state(state);
        }

        auto is_locked() const -> bool;

        /// Return whether this lock is being held by the current thread.
        auto is_held() const -> bool;

        auto holder() const -> task::Thread *;

        auto assert_held() const -> void;

        USTL_NO_MOVEABLE_AND_COPYABLE(SpinLock);

      private:
        constexpr static auto const kIsAutoPreemptDisable = !!(Options & Option::DisablePreempt);

        /// Nothing stands behind Option::DisableIrq: acquire() never touches
        /// the interrupt state, so a lock declared with it would be no safer
        /// than a plain one while reading as though it were. Interrupt-safe
        /// sections go through acquire_irq_save()/release_irq_restore(),
        /// which keep the saved state on the caller's stack, where nesting
        /// still works. Refuse to compile rather than quietly do nothing.
        static_assert(!(Options & Option::DisableIrq),
                      "Option::DisableIrq is not implemented; use "
                      "acquire_irq_save()/release_irq_restore() instead");

        ustl::sync::AtomicUsize m_lockval;
    };

} // namespace spinlock

using SpinLock = spinlock::SpinLock<spinlock::kDefaultOptions>;
using MonitoredSpinLock = spinlock::SpinLock<spinlock::kMonitoredOptions>;
using NoTraceSpinLock = spinlock::SpinLock<spinlock::kNoTraceOptions>;

LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::IrqSave, arch::AutoInterruptDisabler);
LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::PreemptDisable, task::AutoPreemptDisabler);

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SPIN_LOCK_HPP