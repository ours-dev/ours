/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Which nodes exist, and in what state: the table every node is found by.
///
/// The machine's, not any node's. A node is one domain's memory and the CPUs
/// working on it; which domains there are, and which of them are online or own
/// memory, is a fact about all of them, so it lives here at file scope behind
/// free functions and not as static members of PmNode.
#ifndef OURS_MM_NODE_TABLE_HPP
#define OURS_MM_NODE_TABLE_HPP 1

#include <ours/assert.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

#include <ustl/traits/is_invocable.hpp>
#include <ustl/util/move.hpp>

namespace ours::mm {
class PmNode;

/// The states a node can be in, each a mask over the node ids. Whether a node
/// has memory of a zone type is not a state: it is whether PmZone::at() has a
/// zone for it.
enum class NodeState : u32 {
    /// Owns memory: one of the nodes an allocation may fall back to, and so
    /// the set the fallback orders are derived over.
    Memory,

    /// Brought up: its PmNode is initialised and serves allocations.
    Online,

    /// Named by the firmware or by the boot memory map, before any PmNode
    /// exists. Bring-up makes a node of each.
    Possible,

    /// The number of states.
    Count,
};

/// The node of id |nid|, or null when none was brought up there. |nid| is
/// below MAX_NODES. A node is found from the moment bring-up registers it,
/// a little before it is Online.
[[nodiscard]]
auto find_node(NodeId nid) -> PmNode *;

/// The nodes in |state|.
[[nodiscard]]
auto node_mask(NodeState state) -> NodeMask const &;

/// Puts node |nid| in |state|, or takes it out. Bring-up and the firmware
/// parse (SRAT) call it, before any node is used; nothing changes a state
/// after bring-up today.
auto set_node_state(NodeId nid, NodeState state, bool on = true) -> void;

/// How many nodes were brought up.
[[nodiscard]]
auto num_nodes() -> usize;

[[nodiscard]]
FORCE_INLINE
inline auto node_online_mask() -> NodeMask const & {
    return node_mask(NodeState::Online);
}

[[nodiscard]]
FORCE_INLINE
inline auto node_possible_mask() -> NodeMask const & {
    return node_mask(NodeState::Possible);
}

/// The nodes that own memory: the ones worth falling back to.
[[nodiscard]]
FORCE_INLINE
inline auto node_memory_mask() -> NodeMask const & {
    return node_mask(NodeState::Memory);
}

[[nodiscard]]
FORCE_INLINE
inline auto num_online_nodes() -> usize {
    return node_online_mask().count();
}

[[nodiscard]]
FORCE_INLINE
inline auto num_possible_nodes() -> usize {
    return node_possible_mask().count();
}

/// Calls |f| with the id of every node in |state|. A callback returning void
/// visits every node; one returning a value stops at the first result that
/// tests true and yields it.
template <typename F>
    requires ustl::traits::Invocable<F, NodeId>
auto for_each_nid(NodeState const state, F &&f) -> ustl::traits::InvokeResultT<F, NodeId> {
    return node_mask(state).for_each(ustl::forward<F>(f));
}

/// Calls |f| with every node in |state|, which must be a state only a node
/// that was brought up can be in (Online or Memory): Possible names ids that
/// may have no node.
template <typename F>
    requires ustl::traits::Invocable<F, PmNode &>
auto for_each_node(NodeState const state, F &&f) -> ustl::traits::InvokeResultT<F, PmNode &> {
    return node_mask(state).for_each([&f](NodeId const nid) {
        auto *const node = find_node(nid);
        DEBUG_ASSERT(node, "node {} is in a state only a node brought up can be in, and is not",
                     nid);
        return f(*node);
    });
}

namespace priv {
    /// Enters |node| in the table at its id. Node bring-up alone calls it,
    /// once per node, before the node is marked Online; nothing removes one.
    auto register_node(NodeId nid, PmNode &node) -> void;
} // namespace priv

} // namespace ours::mm

#endif // #ifndef OURS_MM_NODE_TABLE_HPP
