/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// How this lock waits, and why it waits that way.
///
/// The lock word holds kUnlockedState when free and the holder's Thread*
/// otherwise, so acquiring is one compare-exchange of 0 -> self. That much is
/// ordinary. What follows is about the wait, which is where the cost of a
/// contended lock actually lives.
///
/// Everything below rests on one hardware rule: a core may read a cache line
/// that other cores also hold, but to *write* one it must first own it
/// exclusively, which means invalidating every other copy and waiting for the
/// acknowledgements. Reads share; writes serialise.
///
/// -- Why the wait loop reads instead of exchanging ------------------------
///
/// A compare-exchange is a read-modify-write, so the hardware has to take the
/// line exclusively for the whole operation -- even on the attempts that
/// decide not to write anything. A wait loop built out of exchanges therefore
/// has every waiter tearing the line out of every other waiter's cache, and
/// none of those attempts can succeed while the lock is held. Three costs
/// come out of that, in rising order of how much they hurt:
///
///   1. Each invalidation turns the next read into a cache miss: tens of
///      cycles where an L1 hit would have been a handful.
///   2. Only one core can own the line at a time, so the attempts serialise.
///      Eight spinners are not eight cores spinning in parallel, they are one
///      cache line being dragged around a ring of eight.
///   3. The holder is in that queue too. Releasing the lock is a write, so it
///      needs the line exclusively like everyone else -- and the more waiters
///      there are, the longer the holder waits to get it. Contention stretches
///      the critical section, which produces more waiters, which stretches it
///      further. This is why a contended spin lock degrades off a cliff
///      rather than gradually.
///
/// So the loop waits on a plain relaxed load. The line settles in Shared on
/// every waiter, reads hit L1, and the interconnect goes quiet until the
/// holder writes the release -- at which point every copy is invalidated once
/// and the waiters re-read, find it free, and try the exchange. Head and tail
/// pay for a line transfer; the middle pays nothing. The point is less that
/// waiters read faster and more that they stop obstructing the holder.
///
/// -- Why preemption is disabled outside the loop -------------------------
///
/// try_acquire() has to enable preemption again when it fails, so driving the
/// wait through it meant two updates of the preemption counter per failed
/// round, and a window between the enable and the next disable in which the
/// waiter could be preempted away mid-spin. acquire() disables once, for the
/// whole wait: its caller is going to hold the lock, so there is nothing to
/// roll back.
///
/// -- Why the exchange names its failure order ----------------------------
///
/// Acquire on success is what pairs with the Release store in release().
/// Failure is a different question: nothing was acquired, so nothing needs
/// ordering, and Relaxed is right. It has to be said out loud because the
/// default is SeqCst -- which __atomic_compare_exchange does not permit above
/// the success order, and which would put a full barrier on the one path that
/// runs most.
///
/// Even uncontended, a lock-prefixed instruction is not free: x86 gives it
/// full-barrier semantics, so it drains the store buffer before completing.
/// That is most of the ~20 cycles it costs with the line already local,
/// against ~4 for an ordinary load.
///
/// -- What this shape still does not solve --------------------------------
///
/// Fairness. Waiters all leave the read loop together and race on the
/// exchange, and whoever wins is whoever the coherence fabric happened to
/// serve first -- arrival order is not recorded anywhere. A core in a tight
/// acquire/release loop is favoured further still, because it releases with
/// the line already in its own cache and can re-take it before anyone else
/// has even re-read it. A waiter can lose arbitrarily many rounds.
///
/// Fixing that means recording arrival, which is what a ticket lock does: a
/// counter handed out by fetch_add, and a second one naming whose turn it is.
/// It costs the holder identity currently kept in the lock word, so it is a
/// design decision rather than a drop-in change.

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
        // try_acquire's caller has no loop to absorb that. See the file header
        // for the two memory orders.
        //
        // Reset every call: a failed exchange writes the observed value back
        // into old_state, so reusing it would compare against the last
        // holder's pointer and fail again by construction.
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

        // Once, for the whole wait -- see the file header.
        if constexpr (kIsAutoPreemptDisable) {
            current_thread->preemption_state().disable_preemption();
        }

        while (!do_lock(current_thread)) {
            // Test-and-test-and-set: wait on a load, not on the exchange.
            // The hint comes first because reaching here means the exchange
            // just failed, so the lock is certainly held right now and there
            // is nothing to re-read yet.
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
