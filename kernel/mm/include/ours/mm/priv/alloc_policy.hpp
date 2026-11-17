/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOC_POLICY_HPP
#define OURS_MM_PRIV_ALLOC_POLICY_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>

#include <ustl/rc.hpp>

#define MEMORY_ALLOCATION_POLICY 1

namespace ours::mm {
/// A thread's NUMA placement policy: where an allocation that names no node
/// goes.
///
/// Immutable once made, so that one policy can be shared by every thread that
/// holds it and read without a lock of its own. What moves from one
/// allocation to the next -- how far an Interleave has got -- is the
/// thread's, kept in its AllocState and handed to place() as a turn.
class AllocPolicy: public ustl::RefCounted<AllocPolicy, ustl::sync::ThreadSafePolicy> {
    using Self = AllocPolicy;

  public:
    enum class Kind {
        /// No policy of its own: the calling CPU's node first, then any.
        Default,

        /// One node first, then any.
        Preferred,

        /// Each allocation from the next of a set of nodes in turn, then any.
        Interleave,

        /// Only a set of nodes, nearest the calling CPU's first, and nothing
        /// else even when they are full.
        Bind,

        /// The calling CPU's node first, then any: Default's placement, said
        /// on purpose.
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

    AllocPolicy() = default;

    [[nodiscard]]
    static auto local() -> Self {
        return Self{Kind::Local, INVALID_NODE_ID, NodeMask{}};
    }

    [[nodiscard]]
    static auto preferred(NodeId const node) -> Self {
        DEBUG_ASSERT(node < MAX_NODES, "node {} is outside the node table", node);
        return Self{Kind::Preferred, node, NodeMask{}};
    }

    [[nodiscard]]
    static auto bind(NodeMask const &nodes) -> Self {
        return Self{Kind::Bind, INVALID_NODE_ID, nodes};
    }

    [[nodiscard]]
    static auto interleave(NodeMask const &nodes) -> Self {
        return Self{Kind::Interleave, INVALID_NODE_ID, nodes};
    }

    FORCE_INLINE
    auto kind() const -> Kind {
        return m_kind;
    }

    /// The placement of an allocation the calling CPU's node |local| makes,
    /// which its caller allows to draw from |allowed|, and which is the
    /// |turn|th the thread has handed this policy. The resolver asks it
    /// (resolve_request()), for an allocation that names no node and is not
    /// confined to its home by Gaf::ThisNode.
    ///
    /// Every answer keeps to |allowed|: the home is |local| or a node of
    /// |allowed|, and the nodes are |allowed| or fewer. Preferred moves the
    /// home to its node when |allowed| has it. Bind leaves the home where it
    /// is, so the walk keeps its nearest-first order, and narrows the nodes to
    /// its own; when none of them is allowed, nothing is, and the allocation
    /// fails. Interleave moves the home to the next of its allowed nodes by
    /// |turn|, and leaves the nodes, so a full node falls back as Preferred
    /// does. Default and Local leave both.
    [[nodiscard]]
    auto place(NodeId const local, NodeMask const &allowed, usize const turn) const -> Placement {
        switch (m_kind) {
            case Kind::Preferred:
                if (allowed.test(m_preferred)) {
                    return Placement{.home = m_preferred, .nodes = allowed};
                }
                break;
            case Kind::Bind:
                return Placement{.home = local, .nodes = allowed & m_nodes};
            case Kind::Interleave: {
                auto const round = allowed & m_nodes;
                if (round.any()) {
                    return Placement{.home = nth_node(round, turn % round.count()),
                                     .nodes = allowed};
                }
                break;
            }
            case Kind::Default:
            case Kind::Local:
            case Kind::NumPolicies:
                break;
        }
        return Placement{.home = local, .nodes = allowed};
    }

  private:
    AllocPolicy(Kind const kind, NodeId const preferred, NodeMask const &nodes)
        : m_kind(kind), m_preferred(preferred), m_nodes(nodes) {}

    /// The |n|th node of |nodes|, counting from the lowest id; |n| is below
    /// nodes.count().
    [[nodiscard]]
    static auto nth_node(NodeMask const &nodes, usize const n) -> NodeId {
        auto found = INVALID_NODE_ID;
        usize seen = 0;
        nodes.for_each([n, &found, &seen](NodeId const node) {
            if (seen == n) {
                found = node;
            }
            seen += 1;
        });
        return found;
    }

    Kind const m_kind = Kind::Default;

    /// Preferred's node.
    NodeId const m_preferred = INVALID_NODE_ID;

    /// Bind's nodes, and Interleave's.
    NodeMask const m_nodes;
};
} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ALLOC_POLICY_HPP
