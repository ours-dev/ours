/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The order a node's allocations should fall back through, derived from the
/// machine's distance matrix.
///
/// This is the only thing anyone needs the distances for, and it is why the
/// matrix exists at all. It lives here rather than in the page allocator
/// because "nearest first" is a statement about the machine, not about zones:
/// the allocator's job is to walk an order it was given, and deriving that
/// order was the last place it had to know what a distance is.
///
/// Asked once, while bringing up the nodes, and never again.
#ifndef NUMA_FALLBACK_ORDER_HPP
#define NUMA_FALLBACK_ORDER_HPP 1

#include <array>

#include <ours/node_mask.hpp>
#include <ours/types.hpp>
#include <ustl/collections/static_vec.hpp>

namespace numa {
using ours::MAX_NODES;
using ours::NodeId;
using ours::NodeMask;

/// One node's order: every other member, nearest first.
///
/// Fixed capacity, one hop per other node, so overflowing it means a node was
/// listed twice.
using FallbackOrder = ustl::collections::StaticVec<NodeId, MAX_NODES - 1>;

/// Every node's, indexed by node id. A node outside |nodes| keeps an empty
/// row.
using FallbackOrders = std::array<FallbackOrder, MAX_NODES>;

/// Fills |orders| with one row per node in |nodes|: the nodes in |members|
/// other than that one, nearest first by Topology::distance(), each listed
/// once.
///
/// Two sets, because they answer different questions. |nodes| is who needs an
/// order -- for the page allocator, every node it built, whether or not that
/// node owns a single frame. |members| is who is worth falling back *to*: the
/// nodes that own memory. A node in |nodes| but not in |members| still gets a
/// full row, and that is the case that matters: it is a node with no memory of
/// its own, whose allocations have nowhere to go except other nodes.
///
/// Nothing here knows how either set was arrived at, which is the point: the
/// caller states who the players are and this answers how far apart they are.
///
/// Out-parameter rather than a return value because the whole table is
/// MAX_NODES rows wide and the caller is bring-up code on a kernel stack: it
/// decides where several kilobytes of table lives, and today it keeps it out
/// of its frame entirely.
///
/// Ties go to the lower node id, because the scan takes a new nearest only on
/// a strictly smaller distance and walks the members in ascending order. That is
/// not a preference, just a rule that makes the result reproducible: an
/// unfilled matrix is all zeroes, so every node is equidistant and the order
/// degenerates to node-id order.
auto build_fallback_orders(NodeMask const &nodes, NodeMask const &members, FallbackOrders *orders)
    -> void;

} // namespace numa

#endif // #ifndef NUMA_FALLBACK_ORDER_HPP
