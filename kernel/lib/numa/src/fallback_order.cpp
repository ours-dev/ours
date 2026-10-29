/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <numa/fallback_order.hpp>
#include <numa/topology.hpp>

#include <ours/assert.hpp>
#include <ustl/limits.hpp>

namespace numa {
namespace {
    /// The nearest member not already in |order|, or MAX_NODES when there is
    /// none left.
    ///
    /// A linear scan per hop rather than one sort of the row: it is
    /// O(members^2) on a count that is 32 at most and is paid once per boot,
    /// and a sort would need somewhere to put the keys. Ties go to the lower
    /// id, because `>` takes a new nearest only on a strictly smaller
    /// distance.
    auto nearest_unvisited(NodeId const from, NodeMask const &members, NodeMask const &visited)
        -> NodeId {
        auto result = NodeId(MAX_NODES);
        auto nearest = ustl::NumericLimits<usize>::max();

        members.for_each([&](NodeId const to) {
            if (visited.test(to)) {
                return;
            }

            auto const distance = Topology::distance(from, to);
            if (nearest > distance) {
                result = to;
                nearest = distance;
            }
        });

        return result;
    }
} // namespace

auto build_fallback_orders(NodeMask const &nodes, NodeMask const &members,
                           FallbackOrders *const orders) -> void {
    DEBUG_ASSERT(orders, "nowhere to put the orders");

    nodes.for_each([&members, orders](NodeId const from) {
        auto &order = (*orders)[from];
        order.clear();

        // |from| is its own first choice and is never a hop, so it starts out
        // visited -- which is also what makes a |from| outside |members| come
        // out right, rather than listing itself. Everything else joins
        // |visited| as it is appended, which is what keeps each member to one
        // hop.
        NodeMask visited;
        visited.set(from);

        while (true) {
            auto const next = nearest_unvisited(from, members, visited);
            if (next == MAX_NODES) {
                break;
            }

            visited.set(next);
            auto const appended = order.push_back(next);
            DEBUG_ASSERT(appended, "node {}'s order overflowed its {} hop capacity", from,
                         MAX_NODES - 1);
        }
    });
}

} // namespace numa
