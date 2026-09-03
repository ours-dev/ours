/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// How this lock waits, and why it waits that way.
///
/// The lock word holds UNLOCKED_STATE when free and one plus the holder's CPU
/// number otherwise, so acquiring is one compare-exchange of 0 -> one plus the
/// calling CPU's number. That much is ordinary. What follows is about the
/// wait, which is where the cost of a contended lock actually lives.
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
/// -- Why the lock leaves preemption to the Guard --------------------------
///
/// It used to be this lock's job: acquire() disabled preemption once for the
/// whole wait, because driving the wait through try_acquire() meant two
/// updates of the preemption counter per failed round and a window between
/// them in which the waiter could be preempted away mid-spin. That state
/// keeping now belongs to the Guard callers wrap around the lock, which saves
/// and restores it through the LOCKDEP_STATE_SAVER declarations in
/// spin-lock.hpp; the lock itself is bare. Note the corollary: until a caller
/// actually goes through such a Guard, holding one of these disables nothing.
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

#include <arch/spin_hint.hpp>
#include <gktl/counter.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>

using ustl::sync::MemoryOrder;

namespace {
GKTL_COUNTER(SP_SPINLOCK_NUM_HELD, "sync.spinlock.num_held");
} // namespace

namespace ours::sync {
namespace spinlock {
    constexpr static auto const UNLOCKED_STATE = 0u;

    /// The state the word takes when `cpu` holds the lock: one plus the CPU
    /// number, so that zero stays free to name "unlocked" -- CPU 0 legitimately
    /// reads as one.
    constexpr static auto holder_state(CpuNum cpu) -> usize {
        return usize(cpu) + 1;
    }

    template <Option Options>
    SpinLock<Options>::SpinLock() : m_lockval(UNLOCKED_STATE) {}

    template <Option Options>
    SpinLock<Options>::~SpinLock() {
        DEBUG_ASSERT(!is_locked());
    }

    template <Option Options>
    auto SpinLock<Options>::do_lock(usize holder) -> bool {
        // strong, not weak: weak may fail spuriously on a free lock, and
        // try_acquire's caller has no loop to absorb that. See the file header
        // for the two memory orders.
        //
        // Reset every call: a failed exchange writes the observed value back
        // into old_state, so reusing it would compare against the last
        // holder's state and fail again by construction.
        usize old_state = UNLOCKED_STATE;
        if (!m_lockval.compare_exchange_strong(old_state, holder, MemoryOrder::Acquire,
                                               MemoryOrder::Relaxed)) {
            return false;
        }

        SP_SPINLOCK_NUM_HELD.add(1);
        return true;
    }

    template <Option Options>
    auto SpinLock<Options>::try_acquire() -> bool {
        return do_lock(holder_state(mm::CpuLocal::current_cpu()));
    }

    template <Option Options>
    auto SpinLock<Options>::acquire() -> void {
        // The ownership token is the running CPU, read out of its per-CPU
        // unit; the boot CPU is CPU 0 by construction and reads as such even
        // before that unit is attached, so nothing here needs Thread::Current
        // to be live.
        //
        // Re-acquiring from the holder's own CPU can never succeed, so without
        // this the CPU just spins here forever with nothing to show for it.
        DEBUG_ASSERT(!is_held());

        while (!do_lock(holder_state(mm::CpuLocal::current_cpu()))) {
            // Test-and-test-and-set: wait on a load, not on the exchange.
            // The hint comes first because reaching here means the exchange
            // just failed, so the lock is certainly held right now and there
            // is nothing to re-read yet.
            do {
                arch::spin_hint();
            } while (m_lockval.load(MemoryOrder::Relaxed) != UNLOCKED_STATE);
        }

        assert_held();
    }

    template <Option Options>
    auto SpinLock<Options>::release() -> void {
        assert_held();

        SP_SPINLOCK_NUM_HELD.add(-1);

        m_lockval.store(UNLOCKED_STATE, MemoryOrder::Release);
    }

    template <Option Options>
    auto SpinLock<Options>::is_locked() const -> bool {
        // Just observes the state of this lock. It does not change the state
        // therefore using relaxed memory order.
        return m_lockval.load(MemoryOrder::Relaxed) != UNLOCKED_STATE;
    }

    template <Option Options>
    auto SpinLock<Options>::is_held() const -> bool {
        return holder_state(mm::CpuLocal::current_cpu()) == m_lockval.load(MemoryOrder::Relaxed);
    }

    template <Option Options>
    auto SpinLock<Options>::assert_held() const -> void {
        ASSERT(m_lockval.load(MemoryOrder::Relaxed) == holder_state(mm::CpuLocal::current_cpu()));
    }

    template <Option Options>
    auto SpinLock<Options>::num_held() -> usize {
        return SP_SPINLOCK_NUM_HELD.sum_across_all_cpus();
    }

    template <Option Options>
    auto SpinLock<Options>::num_held_cpu() -> usize {
        return SP_SPINLOCK_NUM_HELD.get();
    }

    template class SpinLock<DEFAULT_OPTIONS>;
    template class SpinLock<MONITORED_OPTIONS>;
    template class SpinLock<NO_TRACE_OPTIONS>;

} // namespace spinlock
} // namespace ours::sync
