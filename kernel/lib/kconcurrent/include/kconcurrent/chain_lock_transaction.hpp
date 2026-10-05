/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KCONCURRENT_CHAIN_LOCK_TRANSACTION_HPP
#define KCONCURRENT_CHAIN_LOCK_TRANSACTION_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <concurrent/chain_lock.hpp>
#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/types.hpp>

namespace kconcurrent {
using concurrent::ChainLock;
using concurrent::ChainLockToken;
using concurrent::ChainLockTokenGenerator;

/// The kernel half of the chain-lock protocol.
///
/// `concurrent::ChainLock` is the framework: the state word, the token
/// comparison and the three outcomes. It is deliberately environment-free so
/// it builds and is tested on the host, which means it cannot answer the two
/// questions the protocol still needs answered -- *which* token is the caller
/// operating under, and is the caller allowed to be taking this lock at all.
/// This is the specialization that answers them, using the things only the
/// kernel has: a per-CPU slot, an interrupt state, and a spin hint.
///
/// ## What a transaction is for
///
/// One transaction spans one whole attempt-and-retry cycle of a graph walk,
/// not one lock and not one successful walk. That distinction is the entire
/// starvation argument: the token is drawn once, in the constructor, and a
/// back-off releases locks *inside* the transaction's lifetime. So the retry
/// re-walks under the same token and the operation only ever grows older
/// relative to the rest of the system, until it is the oldest live claim and
/// nothing can displace it. Destroying the transaction and making a new one
/// per attempt would draw a fresh token each time and turn the protocol into
/// a livelock -- see the note in chain_lock.hpp.
///
/// The intended shape at a call site is therefore:
///
///     ChainLockTransaction transaction;
///     for (;;) {
///         if (walk_the_chain(transaction)) {
///             break;                  // every lock held; mutate now
///         }
///         // walk_the_chain released everything it held before returning
///         // false; the token survives, so just go round again.
///     }
///
/// ## Why interrupts are off for the whole transaction
///
/// A chain lock is a spin lock, so an interrupt handler that reached for one
/// held by the CPU it interrupted would spin against a token that cannot be
/// released until the handler returns. The disabler is held for the
/// transaction's whole life rather than per-lock because the window that
/// matters is not "while acquiring" but "while any lock is held", and a
/// transaction holds locks across acquisitions by construction.
///
/// It is also what makes a per-CPU current-transaction slot correct. With
/// interrupts off the CPU cannot be preempted, so the running thread cannot
/// change while a transaction is active and "the transaction this CPU is
/// running" and "the transaction this thread is running" are the same thing.
/// A per-CPU slot is the cheaper of the two to reach from an assertion buried
/// in code that was handed no transaction.
class ChainLockTransaction {
    using Self = ChainLockTransaction;

  public:
    /// The spin-loop hint the kernel passes down to the framework half.
    ///
    /// A tiny functor rather than a function pointer so the call inlines to
    /// the `pause` it stands for; `ChainLock` takes it as an ordinary
    /// parameter, so nothing about the lock's type changes with it.
    struct Relax {
        FORCE_INLINE
        auto operator()() const -> void {
            arch::spin_hint();
        }
    };

    /// Draws this operation's token and makes it the CPU's active
    /// transaction. Interrupts are off from here until the destructor.
    ChainLockTransaction();

    ~ChainLockTransaction();

    ChainLockTransaction(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    /// The transaction the current CPU is running, or null.
    ///
    /// Null is a legitimate answer -- most kernel code takes no chain locks
    /// -- so callers that require one assert on it themselves rather than
    /// having this accessor do it for them.
    static auto current() -> Self *;

    FORCE_INLINE
    auto token() const -> ChainLockToken {
        return m_token;
    }

    /// The three wrappers below re-state the framework half's own capability
    /// annotations against the caller's `lock` parameter. Without them the
    /// analysis sees ChainLock::acquire_first_in_chain() take a capability
    /// that never comes back out of acquire_first(), and release() dropping
    /// one it never saw taken.
    ///
    /// Takes the first lock of a walk, spinning until it is ours.
    ///
    /// Cannot back off, because nothing is held yet to release. The framework
    /// half cannot check that claim -- it sees one lock, not the set -- so it
    /// is checked here, where the count lives.
    TA_ACQUIRE(lock)
    auto acquire_first(ChainLock &lock) -> void {
        DEBUG_ASSERT(m_num_locks_held == 0,
                     "acquire_first() with locks already held; use acquire()");
        lock.acquire_first_in_chain(m_token, Relax{});
        note_acquired();
    }

    /// Takes a subsequent lock of a walk.
    ///
    /// Returns false when an older transaction holds it, which is not a
    /// failure but an instruction: release everything and re-walk. The
    /// caller cannot ignore it -- there is no lock to release and no
    /// invariant to trust -- hence [[nodiscard]].
    [[nodiscard]]
    TA_TRY_ACQUIRE(true, lock) auto acquire(ChainLock &lock) -> bool {
        DEBUG_ASSERT(!m_has_mutated,
                     "a chain lock acquired after the walk began mutating; the whole "
                     "set has to be held before anything is written");
        if (!lock.acquire_or_backoff(m_token, Relax{})) {
            return false;
        }
        note_acquired();
        return true;
    }

    TA_RELEASE(lock)
    auto release(ChainLock &lock) -> void {
        DEBUG_ASSERT(m_num_locks_held > 0, "releasing a chain lock this transaction never took");
        lock.release(m_token);
        note_released();
    }

    /// Marks the point where the walk is complete and writing begins.
    ///
    /// The rule it enforces is the half of the protocol the framework cannot
    /// see: while only part of the set is held, the state under those locks
    /// may be read but must not be written, because a back-off unwinds by
    /// dropping locks and has no way to undo writes. Calling this after the
    /// last acquire turns a violation of that rule -- one more acquire, or a
    /// back-off after mutation started -- into an assertion instead of state
    /// left visible to whoever displaced us.
    ///
    /// Debug-only bookkeeping, so it compiles to nothing in a release
    /// kernel and callers need not guard the call.
    FORCE_INLINE
    auto begin_mutation() -> void {
#if KERNEL_DEBUG_LEVEL
        m_has_mutated = true;
#endif
    }

    FORCE_INLINE
    auto num_locks_held() const -> usize {
        return m_num_locks_held;
    }

  private:
    FORCE_INLINE
    auto note_acquired() -> void {
        ++m_num_locks_held;
    }

    FORCE_INLINE
    auto note_released() -> void {
        --m_num_locks_held;
    }

    /// Composition, not a base class: a transaction *has* an interrupt state
    /// to restore, it is not a kind of interrupt disabler. Declared first so
    /// it is constructed before anything else here and destroyed last, which
    /// is the order the protocol needs -- interrupts must already be off
    /// before this transaction becomes visible to the CPU, and must not come
    /// back until it is gone.
    arch::AutoInterruptDisabler m_interrupt_disabler;

    ChainLockToken m_token;
    usize m_num_locks_held;

    /// Only read by assertions; see begin_mutation(). Kept unconditionally
    /// rather than under #if so that the class layout does not change with
    /// the debug level -- one bool is not worth two ABIs.
    bool m_has_mutated;

    /// The previous active transaction, restored on destruction.
    ///
    /// Nesting is not expected and the constructor asserts against it, but
    /// the save/restore is written anyway: it costs a word, and it is the
    /// difference between a future nested case being merely wrong and
    /// leaving a dangling pointer in the CPU's slot.
    Self *m_previous;
};

} // namespace kconcurrent

#endif // #ifndef KCONCURRENT_CHAIN_LOCK_TRANSACTION_HPP
