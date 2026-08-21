#include <arch/spin-hint.hpp>
#include <gktl/counter.hpp>
#include <ours/sync/spin-lock.hpp>
#include <ours/task/thread.hpp>

using ours::task::Thread;
using ustl::sync::MemoryOrder;

namespace {
GKTL_COUNTER(sp_spinlock_num_held, "sp.spinlock.num_held");
} // namespace

namespace ours::sync {
namespace spinlock {
    constexpr static auto const kUnlockedState = 0u;

    template <Option Options>
    SpinLock<Options>::SpinLock() : m_lockval(kUnlockedState) {}

    template <Option Options>
    SpinLock<Options>::~SpinLock() {
        DEBUG_ASSERT(!is_locked());
    }

    template <Option Options>
    auto SpinLock<Options>::do_lock(Thread *current) -> bool {
        // strong, not weak: weak may fail spuriously on a free lock, and
        // try_acquire's caller has no loop to absorb that -- it would just be
        // told the lock was taken when it was not.
        //
        // Acquire on success is what pairs with the Release store in
        // release(). Relaxed on failure, spelled out rather than left to the
        // default: nothing is acquired when the exchange fails, and the
        // default is SeqCst, which is both stronger than the success order --
        // which the builtin does not allow -- and a full barrier on the
        // hottest path this lock has.
        usize old_state = kUnlockedState;
        if (!m_lockval.compare_exchange_strong(old_state, reinterpret_cast<usize>(current),
                                               MemoryOrder::Acquire, MemoryOrder::Relaxed)) {
            return false;
        }

        sp_spinlock_num_held.add(1);
        return true;
    }

    template <Option Options>
    auto SpinLock<Options>::try_acquire() -> bool {
        auto const current_thread = Thread::Current::get();

        // Before the exchange, never after: succeeding first would leave an
        // instant where the lock is held and the holder is still preemptible.
        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().disable_preemption();
        }

        if (do_lock(current_thread)) {
            return true;
        }

        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().enable_preemption();
        }
        return false;
    }

    template <Option Options>
    auto SpinLock<Options>::acquire() -> void {
        auto const current_thread = Thread::Current::get();

        // The ownership token is the running thread, and preemption is
        // disabled through it, so this lock needs Thread::Current to be live.
        // start_kernel() establishes that in task::init_early(), well ahead of
        // init_vmm() and everything below it that reaches for a lock, so this
        // only fires if something new gets hoisted above that point.
        DEBUG_ASSERT(current_thread != nullptr);

        // Re-acquiring from the holder itself can never succeed, so without
        // this the CPU just spins here forever with nothing to show for it.
        DEBUG_ASSERT(!is_held());

        // Disabled once for the whole wait, rather than per attempt. Going
        // through try_acquire() every round cost two updates of the preemption
        // counter on each failure, and reopened a window between them where
        // this thread could be preempted away in the middle of spinning.
        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().disable_preemption();
        }

        while (!do_lock(current_thread)) {
            // Test-and-test-and-set. A failing compare-exchange still has to
            // take the cache line exclusively, so spinning on it means every
            // waiter keeps tearing the line away from the others and from the
            // holder trying to release it. Wait on a plain load instead: the
            // line stays Shared on every waiter until the lock actually goes
            // free, and only then does anyone attempt the exchange again.
            do {
                arch::spin_hint();
            } while (m_lockval.load(MemoryOrder::Relaxed) != kUnlockedState);
        }

        assert_held();
    }

    template <Option Options>
    auto SpinLock<Options>::release() -> void {
        assert_held();

        sp_spinlock_num_held.add(-1);

        m_lockval.store(kUnlockedState, MemoryOrder::Release);

        if constexpr (kIsAutoPreemptDisable) {
            Thread::Current::preemption_state().enable_preemption();
        }
    }

    template <Option Options>
    auto SpinLock<Options>::is_locked() const -> bool {
        // Just observes the state of this lock. It does not change the state
        // therefore using relaxed memory order.
        return m_lockval.load(MemoryOrder::Relaxed) != kUnlockedState;
    }

    template <Option Options>
    auto SpinLock<Options>::is_held() const -> bool {
        return Thread::Current::get() == holder();
    }

    template <Option Options>
    auto SpinLock<Options>::holder() const -> Thread * {
        return reinterpret_cast<Thread *>(m_lockval.load(MemoryOrder::Relaxed));
    }

    template <Option Options>
    auto SpinLock<Options>::assert_held() const -> void {
        auto const applicant = reinterpret_cast<usize>(Thread::Current::get());
        ASSERT(m_lockval.load(MemoryOrder::Relaxed) == applicant);
    }

    template <Option Options>
    auto SpinLock<Options>::num_held() -> usize {
        return sp_spinlock_num_held.sum_across_all_cpus();
    }

    template <Option Options>
    auto SpinLock<Options>::num_held_cpu() -> usize {
        return sp_spinlock_num_held.get();
    }

    template class SpinLock<kDefaultOptions>;
    template class SpinLock<kMonitoredOptions>;
    template class SpinLock<kNoTraceOptions>;

} // namespace spinlock
} // namespace ours::sync
