/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOC_POLICY_HPP
#define OURS_MM_PRIV_ALLOC_POLICY_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>

#include <utility>

#include <ustl/rc.hpp>

#define MEMORY_ALLOCATION_POLICY 1

namespace ours::mm {
class AllocPolicy: public ustl::RefCounted<AllocPolicy, ustl::sync::ThreadSafePolicy> {
    using Self = AllocPolicy;

  public:
    enum class Kind {
        Default,
        Preferred,
        Interleave,
        Bind,
        Local,
        NumPolicies,
    };

    /// Where an allocation that names no node goes.
    struct Placement {
        /// The node whose zone order the walk starts from.
        NodeId home;

        /// The nodes it may draw from.
        NodeMask nodes;
    };

    FORCE_INLINE
    auto kind() const -> Kind {
        return m_kind;
    }

    /// The placement of an allocation the calling CPU's node |local| makes,
    /// which its caller allows to draw from |allowed|. The resolver asks it
    /// (resolve_request()), for an allocation that names no node and is not
    /// confined to its home by Gaf::ThisNode.
    ///
    /// Default and Local leave both as they are. Preferred would move the
    /// home, Bind narrow the nodes, Interleave move the home round them; none
    /// is built, and nothing can make a policy of those kinds yet, since
    /// AllocState has no way to set one. This is the shape their answers
    /// take, so that each lands here when it is built.
    [[nodiscard]]
    FORCE_INLINE
    auto place(NodeId const local, NodeMask const &allowed) const -> Placement {
        DEBUG_ASSERT(m_kind == Kind::Default || m_kind == Kind::Local,
                     "placement policy kind {} is not built yet", std::to_underlying(m_kind));
        return Placement{.home = local, .nodes = allowed};
    }

  private:
    Kind m_kind = Kind::Default;

    [[maybe_unused]]
    NodeId m_prev_nid; // Use for Policy::Interleave.
    [[maybe_unused]]
    NodeId m_home_nid; // Use for Policy::Bind.

    NodeMask m_nodes;
    NodeMask m_allowed_nodes;
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ALLOC_POLICY_HPP
