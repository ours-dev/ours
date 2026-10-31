/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The machine's NUMA tables: how far apart two nodes are, and which node each
/// CPU is on.
///
/// This library is where NUMA lives, so that the layers around it do not have
/// to. Whatever can read the firmware fills these in -- on x86 the SRAT and
/// SLIT parse in kernel/arch/x86/mm/srat.cpp -- and the node layer reads them
/// once, at bring-up, to learn each node's CPUs and the order it should fall
/// back through. The page allocator asks nothing of this on any path an
/// allocation takes.
///
/// It was nested inside the page allocator's node class, which made an arch
/// file include mm's header to record a distance; then it was moved out of the
/// class but left inside mm, which moved the code without moving the concept.
/// See docs/changes/261028/mm.md.
#ifndef NUMA_TOPOLOGY_HPP
#define NUMA_TOPOLOGY_HPP 1

#include <array>

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

namespace numa {
using ours::CpuNum;
using ours::MAX_CPU;
using ours::MAX_NODES;
using ours::NodeId;

/// The firmware's view of the machine, and the running placement derived from
/// it.
///
/// Every accessor is static because there is one machine. The tables are
/// private so that the bounds checks below are the only way in: a node id or a
/// CPU number out of range is a caller error, and indexing the arrays directly
/// would make it a silent one.
class Topology {
  public:
    /// The firmware's distance between two nodes (the SLIT, or what the SRAT
    /// parse fills in). Zero until someone sets it.
    FORCE_INLINE
    static auto distance(NodeId const x, NodeId const y) -> usize {
        DEBUG_ASSERT(x < MAX_NODES && y < MAX_NODES);
        return DISTANCE[x][y];
    }

    FORCE_INLINE
    static auto set_distance(NodeId const x, NodeId const y, usize const distance) -> void {
        DEBUG_ASSERT(x < MAX_NODES && y < MAX_NODES);
        DISTANCE[x][y] = distance;
    }

    /// The node the firmware put |cpu| on -- what a node's hard affinity is
    /// built from. Unlike cpu_to_node(), a run-time move does not change it.
    FORCE_INLINE
    static auto firmware_node(CpuNum const cpu) -> NodeId {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        return FIRMWARE_NODE_OF_CPU[cpu];
    }

    /// Records what the firmware declared for |cpu|. Only while bringing up,
    /// from whatever reads the firmware.
    ///
    /// It used to write a second table as well -- the running placement -- and
    /// that one belongs to the node layer, which seeds it from here once and
    /// owns it afterwards (ours::mm::NodePlacement). A CPU moved at run time
    /// must not change what the firmware said, because a node's hard affinity
    /// is built from this.
    FORCE_INLINE
    static auto set_firmware_node(CpuNum const cpu, NodeId const nid) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        FIRMWARE_NODE_OF_CPU[cpu] = nid;
    }

  private:
    static std::array<std::array<usize, MAX_NODES>, MAX_NODES> DISTANCE;

    /// Starts out on MAX_NODES, the "the firmware said nothing" sentinel;
    /// see src/topology.cpp.
    static std::array<NodeId, MAX_CPU> FIRMWARE_NODE_OF_CPU;
};

} // namespace numa

#endif // #ifndef NUMA_TOPOLOGY_HPP
