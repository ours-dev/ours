/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_ALLOC_STATE_HPP
#define OURS_MM_ALLOC_STATE_HPP 1

#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/priv/alloc_policy.hpp>
#include <ours/node_mask.hpp>

#include <atomic>

#include <ustl/rc.hpp>

namespace ours::mm {
/// Replaces the calling thread's placement policy with |policy|, none when
/// null, and returns the one it replaced, for the caller to let go of or put
/// back.
///
/// The calling thread's only: a thread's policy is its own to set, so that its
/// own allocations are the only ones that need to read it safely against a
/// replacement (docs/repairs/261116/mm.md item 1). It is replaced under the
/// thread's resource lock all the same, the rule for every reader that is not
/// the thread itself. An Interleave starts again from its first node.
auto set_thread_policy(ustl::Arc<AllocPolicy> policy) -> ustl::Arc<AllocPolicy>;

class AllocState {
    using Self = AllocState;

  public:
    auto enable() -> void {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        DEBUG_ASSERT(m_disable_count);
        m_disable_count -= 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    auto disable() -> void {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        m_disable_count += 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    auto is_enabled() const -> bool {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return !m_disable_count;
    }

    auto policy() -> ustl::Arc<AllocPolicy> {
        return m_policy;
    }

    /// The turn of the next allocation the thread's policy places: how many
    /// it has placed since the policy was set (AllocPolicy::place()).
    auto policy_turn() const -> usize {
        return m_policy_turns;
    }

    /// Counts one allocation the policy placed (AllocRequest::placed_by_policy()),
    /// and none it did not: one that carried Gaf::ThisNode takes no turn, so an
    /// Interleave stays even however the two are mixed.
    auto count_policy_turn() -> void {
        m_policy_turns += 1;
    }

  private:
    friend auto set_thread_policy(ustl::Arc<AllocPolicy> policy) -> ustl::Arc<AllocPolicy>;

    volatile u16 m_disable_count = 0;
    ustl::Arc<AllocPolicy> m_policy;

    /// What policy_turn() answers. Written by the thread alone: by its
    /// allocations and by set_thread_policy(), which it alone calls. An
    /// allocation made in interrupt context while the thread is current would
    /// read its policy and count a turn too; nothing allocates there with no
    /// node named today, and there is no in-interrupt query to keep it out.
    usize m_policy_turns = 0;
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_ALLOC_STATE_HPP
