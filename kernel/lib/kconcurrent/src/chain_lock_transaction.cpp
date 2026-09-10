/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/cpu_local.hpp>

#include <kconcurrent/chain_lock_transaction.hpp>

namespace kconcurrent {
namespace {
    /// The transaction the CPU is currently running, or null.
    ///
    /// Per-CPU rather than per-thread, which is only correct because a
    /// transaction holds interrupts off for its whole life: the CPU cannot be
    /// preempted while one is active, so the thread cannot change underneath
    /// it. See the class comment.
    ///
    /// A plain pointer, not an atomic. Only the owning CPU ever reads or
    /// writes its own slot, and it does so with interrupts disabled, so there
    /// is no concurrency here to order against.
    CPU_LOCAL
    ChainLockTransaction *CURRENT_TRANSACTION;
} // namespace

auto ChainLockTransaction::current() -> Self * {
    return ours::mm::CpuLocal::access(CURRENT_TRANSACTION);
}

/// The member initialisers run in declaration order, so m_interrupt_disabler
/// has already masked interrupts by the time this body publishes the
/// transaction to the CPU's slot. That order is required, not incidental: a
/// transaction visible on a CPU that can still take an interrupt is exactly
/// the state where a handler can walk into a chain lock this CPU holds and
/// spin on a token that cannot be released until the handler returns.
ChainLockTransaction::ChainLockTransaction()
    : m_interrupt_disabler()
    , m_token(ChainLockTokenGenerator::next())
    , m_num_locks_held(0)
    , m_has_mutated(false)
    , m_previous(nullptr) {
    auto &slot = ours::mm::CpuLocal::access(CURRENT_TRANSACTION);

    // Nesting would mean two live tokens on one CPU, and the inner one is
    // necessarily younger -- so an inner walk could be told to back off and
    // release locks the outer walk is relying on. If a real need for nesting
    // ever turns up it has to be designed, not allowed to happen by accident.
    DEBUG_ASSERT(slot == nullptr, "a chain lock transaction is already active on this CPU");

    m_previous = slot;
    slot = this;
}

ChainLockTransaction::~ChainLockTransaction() {
    // Every lock has to be gone before the token stops being current. A lock
    // left held here would keep a token in its state word that nothing will
    // ever release, and every later transaction that reaches it would compare
    // itself against a holder that no longer exists: the younger ones back off
    // forever, the older ones spin forever.
    DEBUG_ASSERT(m_num_locks_held == 0, "chain lock transaction ended still holding locks");

    ours::mm::CpuLocal::access(CURRENT_TRANSACTION) = m_previous;

    // m_interrupt_disabler is destroyed after this body, restoring the
    // interrupt state only once the transaction is no longer current.
}

} // namespace kconcurrent
