/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_MEMORY_ALLOCATION_POLICY_HPP
#define OURS_MM_PRIV_MEMORY_ALLOCATION_POLICY_HPP 1

#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>

#include <ustl/rc.hpp>

#define MEMORY_ALLOCATION_POLICY 1

namespace ours::mm {
class MemoryAllocationPolicy
    : public ustl::RefCounted<MemoryAllocationPolicy, ustl::sync::ThreadSafePolicy> {
    using Self = MemoryAllocationPolicy;

  public:
    enum class Kind {
        Default,
        Preferred,
        Interleave,
        Bind,
        Local,
        NumPolicies,
    };

    FORCE_INLINE
    auto kind() const -> Kind {
        return m_kind;
    }

  private:
    Kind m_kind;

    [[maybe_unused]]
    NodeId m_prev_nid; // Use for Policy::Interleave.
    [[maybe_unused]]
    NodeId m_home_nid; // Use for Policy::Bind.

    NodeMask m_nodes;
    NodeMask m_allowed_nodes;
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_MEMORY_ALLOCATION_POLICY_HPP
