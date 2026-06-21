/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PRIV_MEMORY_ALLOCATION_POLICY_HPP
#define OURS_MEM_PRIV_MEMORY_ALLOCATION_POLICY_HPP 1

#include <ours/mem/types.hpp>
#include <ours/mem/node-mask.hpp>

#include <ustl/rc.hpp>

#define MEMORY_ALLOCATION_POLICY 1

namespace ours::mem {
    class MemoryAllocationPolicy: public ustl::RefCounter<MemoryAllocationPolicy> {
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
        auto kind() const -> Kind { return m_kind; }
      private:
        Kind m_kind;

        NnId m_prev_nid; // Use for Policy::Interleave.
        NnId m_home_nid; // Use for Policy::Bind.

        NodeMask m_nodes;
        NodeMask m_allowed_nodes;
    };
} // namespace ours::mem

#endif // #ifndef OURS_MEM_PRIV_MEMORY_ALLOCATION_POLICY_HPP