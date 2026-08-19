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
    auto SpinLock<Options>::try_acquire() -> bool {
        auto const current_thread = Thread::Current::get();
        auto const new_state = reinterpret_cast<usize>(current_thread);

        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().disable_preemption();
        }

        usize old_state = kUnlockedState;
        if (m_lockval.compare_exchange_weak(old_state, new_state, MemoryOrder::Acquire)) {
            sp_spinlock_num_held.add(1);
            return true;
        }

        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().enable_preemption();
        }
        return false;
    }

    template <Option Options>
    auto SpinLock<Options>::acquire() -> void {
        constexpr auto kSpinTimesPerTry = sizeof(usize) << 3;
        while (!try_acquire()) {
            for (usize i = 0; i < kSpinTimesPerTry; ++i) {
                arch::spin_hint();
            }
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
