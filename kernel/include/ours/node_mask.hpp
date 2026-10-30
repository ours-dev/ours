/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// How many memory nodes there can be, and a set of them.
///
/// Beside cpu_mask.hpp rather than under ours/mm/, and shaped the same way,
/// because a node count is a property of the machine and not of the page
/// allocator. It was in ours/mm/node_mask.hpp with the count in
/// ours/mm/cfg.hpp, which meant anything wanting to name a node had to include
/// a memory-management header -- and kernel/lib/numa, which derives the node
/// ordering, cannot do that without depending on the layer it feeds.
#ifndef OURS_NODE_MASK_HPP
#define OURS_NODE_MASK_HPP 1

#include <ours/types.hpp>

#include <ustl/bit.hpp>
#include <ustl/bitset.hpp>

namespace ours {
/// The ceiling on node ids, and the width in bits of a node id.
///
/// Derived from the build's NUMA switches, which is the only thing NUMA about
/// a node: with neither on there is exactly one node and every mask is one bit
/// wide. The switches reach source as the GN booleans `true`/`false` (see
/// //kernel/products:memory), which `#if` reads as 1 and 0.
///
/// MAX_NODES_BITS is computed rather than written out beside the count. It
/// used to be a second `#define` per branch, with a static_assert in
/// pm_frame.hpp whose only job was to catch the two drifting apart; a
/// constexpr cannot drift from the value it is derived from, so the assertion
/// has nothing left to check. The comment that justified writing it by hand --
/// that assembly reads the header and has no bit_width() -- was stale: six .S
/// files include ours/mm/cfg.hpp and none of them names either constant.
#if (defined(MM_NUMA) && MM_NUMA) || (defined(MM_NUMA_EMULATION) && MM_NUMA_EMULATION)
inline constexpr auto MAX_NODES = usize{32};
#else
inline constexpr auto MAX_NODES = usize{1};
#endif

inline constexpr auto MAX_NODES_BITS = usize{ustl::bit_width(MAX_NODES)};

/// Not a node. One past the ceiling, so it can never collide with a real id.
///
/// Note that MAX_NODES itself is also used as a "no node known yet" sentinel
/// in places -- NODE_OF_CPU is filled with it, and `nid >= MAX_NODES` is how a
/// caller says "no preference". Two sentinels for one idea, and this is the
/// one with no reader; see docs/changes/261028/mm.md.
inline constexpr auto INVALID_NODE_ID = NodeId{MAX_NODES + 1};

using NodeMask = ustl::BitSet<MAX_NODES, NodeId>;
} // namespace ours

#endif // OURS_NODE_MASK_HPP
