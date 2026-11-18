/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/mm/node_table.hpp>

#include <array>
#include <utility>

namespace ours::mm {
namespace {
    constexpr usize NUM_STATES = std::to_underlying(NodeState::Count);

    /// One mask per state. Written by bring-up and the firmware parse before
    /// any node is used, and read without a lock after: nothing changes a
    /// state once the nodes are up.
    std::array<NodeMask, NUM_STATES> STATES;

    /// Every node brought up, at its id. Written once per node by
    /// priv::register_node(), at bring-up, before the node is Online.
    std::array<PmNode *, MAX_NODES> NODES;

    usize NUM_NODES;
} // namespace

auto find_node(NodeId const nid) -> PmNode * {
    DEBUG_ASSERT(nid < MAX_NODES, "node id {} is outside the node table", nid);
    return NODES[nid];
}

auto node_mask(NodeState const state) -> NodeMask const & {
    return STATES[std::to_underlying(state)];
}

auto set_node_state(NodeId const nid, NodeState const state, bool const on) -> void {
    DEBUG_ASSERT(nid < MAX_NODES, "node id {} is outside the node table", nid);
    if (on && (state == NodeState::Online || state == NodeState::Memory)) {
        // Every node in these masks is one find_node() answers for:
        // for_each_node() dereferences what it finds.
        DEBUG_ASSERT(NODES[nid] != nullptr, "node {} is given state {} before it is registered",
                     nid, std::to_underlying(state));
    }
    if (on && state == NodeState::Memory) {
        DEBUG_ASSERT(STATES[std::to_underlying(NodeState::Online)].test(nid),
                     "node {} owns memory before it is online", nid);
    }
    STATES[std::to_underlying(state)].set(nid, on);
}

auto num_nodes() -> usize {
    return NUM_NODES;
}

namespace priv {
    auto register_node(NodeId const nid, PmNode &node) -> void {
        DEBUG_ASSERT(nid < MAX_NODES, "node id {} is outside the node table", nid);
        DEBUG_ASSERT(NODES[nid] == nullptr, "node {} is registered twice", nid);
        NODES[nid] = &node;
        NUM_NODES += 1;
    }
} // namespace priv

} // namespace ours::mm
