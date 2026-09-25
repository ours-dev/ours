/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_SPIN_LOCK_HPP
#define OURS_SYNC_SPIN_LOCK_HPP 1

#include <ours/sync/config.hpp>
#include <ours/sync/priv/lock_name_storage.hpp>
#include <ours/task/types.hpp>
#include <ours/types.hpp>

#include <atomic>
#include <tuple>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/interrupt.hpp>
#include <lockdep/state_saver.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::sync {
namespace spinlock {
    /// Two of these are placeholders for machinery this kernel does not have
    /// yet. They are kept because they name real axes of the design they come
    /// from -- Zircon's Monitored/Unmonitored split and its lockdep exclusion
    /// are separate concerns, not two spellings of one -- but neither is read
    /// anywhere today, so do not expect a lock to behave differently for
    /// selecting them.
    enum class Option {
        /// For the lockup detector: a monitored lock times its own critical
        /// section, so a CPU wedged holding one can be spotted. Unimplemented;
        /// there is no lockup detector here yet, and the detector is also why
        /// the axis has to exist at all -- it takes spin locks itself and must
        /// not recurse into its own instrumentation.
        Monitored = 0_bit,

        /// For lockdep: the dependency tracker takes spin locks of its own, so
        /// the locks beneath it have to stay out of the graph. Unimplemented;
        /// lockdep here is still headers over a six-line translation unit, and
        /// spin-lock.cpp does not call into it at all.
        NoTrace = 1_bit,

        /// For the preemption state the caller wants held across the critical
        /// section. Unimplemented by the lock itself: acquiring and releasing
        /// touch neither preemption nor interrupts -- the Guard a caller wraps
        /// around the lock owns that, through the LOCKDEP_STATE_SAVER
        /// declarations at the bottom of this header.
        DisablePreempt = 2_bit,

        /// Unimplemented, and unlike the two above this one would be unsafe to
        /// ignore quietly, so asking for it fails to compile. See the
        /// static_assert in SpinLock.
        DisableIrq = 3_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(Option);

    /// None of these changes what acquire()/release() do to the running
    /// thread's state: the lock is bare, and preemption is the Guard's
    /// concern. Monitored, NoTrace and DisablePreempt only select how much
    /// bookkeeping the lock will eventually do. In particular DisablePreempt
    /// here does not make an option-less Guard hold preemption off: such a
    /// Guard would disable nothing at all, and is refused at compile time --
    /// see lockdep_option_required() below.
    constexpr static auto DEFAULT_OPTIONS = Option::DisablePreempt;

    constexpr static auto MONITORED_OPTIONS = Option::Monitored | Option::DisablePreempt;

    constexpr static auto NO_TRACE_OPTIONS = Option::NoTrace | Option::DisablePreempt;

    template <Option Options>
    class TA_CAPABILITY("SpinLock") SpinLock
        : public priv::LockNameStorage<SpinLock<Options>, LOCK_TRACING_ENABLED> {
        using Self = SpinLock;

      public:
        /// Both are unused: they are here ready for the lockup detector and
        /// lockdep to be wired in, and are what those call sites will test.
        constexpr static auto IS_MONITORED = !!(Options & Option::Monitored);

        constexpr static auto IS_NO_TRACE = !!(Options & Option::NoTrace);

        static auto num_held() -> usize;

        static auto num_held_cpu() -> usize;

        explicit SpinLock();

        ~SpinLock();

        /// Acquire the lock.
        TA_ACQUIRE() auto acquire() -> void;

        /// Try to acquire the lock, return true if success.
        TA_TRY_ACQUIRE(true) auto try_acquire() -> bool;

        /// Release the lock.
        TA_RELEASE() auto release() -> void;

        TA_ACQUIRE() auto acquire_irq_save(::arch::IrqSavedState &state) -> void {
            state = ::arch::local_irq_save_state();
            acquire();
        }

        /// Order matters: the lock has to be dropped before interrupts come
        /// back. Restoring first leaves a window in which an interrupt taken
        /// on this CPU can reach for this same lock and self-deadlock.
        TA_RELEASE() auto release_irq_restore(::arch::IrqSavedState state) -> void {
            release();
            ::arch::local_irq_restore_state(state);
        }

        auto is_locked() const -> bool;

        /// Return whether this lock is being held on the current CPU.
        auto is_held() const -> bool;

        /// TA_ASSERT: tells the analysis the lock is held from here on,
        /// for code that inherits a lock it cannot prove it holds.
        TA_ASSERT() auto assert_held() const -> void;

        USTL_NO_MOVEABLE_AND_COPYABLE(SpinLock);

      private:
        /// The bare exchange, with no bookkeeping around it. acquire() and
        /// try_acquire() both come through here and differ only in what they
        /// do when it reports failure. `holder` is the state the word takes
        /// on success, one plus the holder's CPU number.
        auto do_lock(usize holder) -> bool;

        /// Nothing stands behind Option::DisableIrq: acquire() never touches
        /// the interrupt state, so a lock declared with it would be no safer
        /// than a plain one while reading as though it were. Interrupt-safe
        /// sections go through acquire_irq_save()/release_irq_restore(),
        /// which keep the saved state on the caller's stack, where nesting
        /// still works. Refuse to compile rather than quietly do nothing.
        static_assert(!(Options & Option::DisableIrq),
                      "Option::DisableIrq is not implemented; use "
                      "acquire_irq_save()/release_irq_restore() instead");

        std::atomic<usize> m_lockval;
    };

    /// The state savers a `Guard<SpinLock, ...>` holds for each option tag,
    /// resolved through lockdep::StateSaver.
    ///
    /// They live inside `spinlock`, next to the class template, and not
    /// beside the SpinLock alias in the enclosing ours::sync. StateSaver
    /// finds them by argument-dependent lookup, and the namespace ADL
    /// associates with `SpinLock<Options>` is the one the class template
    /// itself was declared in; ADL does not walk outwards. Declaring them one
    /// level up compiles fine on its own and then fails at every use with
    /// "use of undeclared identifier 'lockdep_get_state_saver'".
    ///
    /// Written by hand rather than through LOCKDEP_STATE_SAVER because
    /// SpinLock is a template: one declaration each covers SpinLock,
    /// MonitoredSpinLock and NoTraceSpinLock. They are declarations only --
    /// StateSaver names the return type through decltype and never calls
    /// them -- which is also what lets them mention task::AutoPreemptDisabler
    /// while it is still incomplete here. Completing it would need
    /// <ours/task/auto_preempt_disabler.hpp>, which includes thread.hpp,
    /// which includes this header.
    template <Option Options>
    auto lockdep_get_state_saver(SpinLock<Options> *, lockdep::IrqSave *)
        -> ::arch::AutoInterruptDisabler;

    template <Option Options>
    auto lockdep_get_state_saver(SpinLock<Options> *, lockdep::PreemptDisable *)
        -> task::AutoPreemptDisabler;

    /// No single RAII type does both, and none should: holding off interrupts
    /// and holding off preemption are independent concerns that happen to be
    /// wanted together. std::tuple composes them without inventing a class,
    /// and its destruction order -- reverse of declaration -- is the one this
    /// needs: preemption is re-enabled first, so a reschedule the flush
    /// delivers still runs with interrupts disabled, which is the state
    /// Scheduler::reschedule_common asserts on entry; interrupts come back
    /// last.
    template <Option Options>
    auto lockdep_get_state_saver(SpinLock<Options> *, lockdep::IrqSaveAndPreemptDisable *)
        -> std::tuple<::arch::AutoInterruptDisabler, task::AutoPreemptDisabler>;

    /// Makes `Guard guard{lock}` on any SpinLock a compile error; see
    /// lockdep::OptionRequired. acquire() touches neither interrupts nor
    /// preemption, so an option-less Guard -- which the deduction guide
    /// produces from the most natural spelling there is -- held the lock with
    /// both enabled.
    ///
    /// Refusing it was chosen over quietly honouring DEFAULT_OPTIONS
    /// (DisablePreempt) as the Guard's default, for three reasons. Preemption
    /// alone is the wrong answer for every lock also taken from interrupt
    /// context -- the run queues, the wait queues, the IPI mailboxes -- so a
    /// default would only have moved the audit from every call site to the
    /// ones that silently needed more. AutoPreemptDisabler reaches the current
    /// thread's PreemptionState, which does not exist yet for the locks the
    /// boot takes before the first thread is set up. And it is what Zircon
    /// does: its SpinLock is registered with LOCK_DEP_POLICY_OPTION, so
    /// `Guard<SpinLock>` names no policy and fails its static_assert.
    ///
    /// What to write instead is the rule the tree follows: `Guard<SpinLock,
    /// IrqSave>` -- every spin lock held with interrupts off, as Zircon holds
    /// all of its own -- and `Guard<SpinLock, IrqSaveAndPreemptDisable>` where
    /// the scope reaches reschedule_common() or wakes a thread, so that the
    /// reschedule a wake asks for waits until the lock is dropped.
    template <Option Options>
    auto lockdep_option_required(SpinLock<Options> *) -> void;

} // namespace spinlock

using SpinLock = spinlock::SpinLock<spinlock::DEFAULT_OPTIONS>;
using MonitoredSpinLock = spinlock::SpinLock<spinlock::MONITORED_OPTIONS>;
using NoTraceSpinLock = spinlock::SpinLock<spinlock::NO_TRACE_OPTIONS>;

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SPIN_LOCK_HPP