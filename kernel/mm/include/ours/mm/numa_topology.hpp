/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The machine's NUMA tables: how far apart two nodes are, and which node each
/// CPU is on.
///
/// Its own header, and not a member of PmNode, because it is neither a
/// property of any one node nor anything the allocator owns. It is an input,
/// filled by whatever can read the firmware -- on x86 the SRAT and SLIT parse
/// in kernel/arch/x86/mm/srat.cpp -- and read afterwards by the allocator, by
/// a node's affinity masks and by the scheduler's view of where a CPU sits.
/// Nesting it inside PmNode made an arch file reach into the page allocator's
/// header to record a distance between two nodes.
#ifndef OURS_MM_NUMA_TOPOLOGY_HPP
#define OURS_MM_NUMA_TOPOLOGY_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours::mm {
/// The machine's NUMA layout, as the firmware describes it: how far apart the
/// nodes are, and which node each CPU belongs to.
///
/// Facts about the machine rather than about any PmNode, which is why they are
/// kept apart from the nodes: the SRAT parse (x86 srat.cpp, InitLevel::PrePmm
/// + 1) records them before a single PmNode exists, and a caller that only
/// wants a node number -- mm::current_node() has dozens -- has no business
/// with zones and free lists. They used to be spread over PmNode's statics
/// (NODE_DISTANCE) and PmNode::Current's (STATIC_/DYNAMIC_CPU_TO_NODE).
///
/// Nested in PmNode so that the NUMA vocabulary stays under one name, the way
/// PmNode::Current already is. What changes a PmNode when a CPU joins or
/// leaves it -- its CPU masks, the per-CPU PmNode pointer -- stays with
/// PmNode::Current; this only holds the tables.
class NumaTopology {
  public:
    /// The firmware's distance between two nodes (the SLIT, or what the SRAT
    /// parse fills in). Zero until someone sets it.
    FORCE_INLINE
    static auto distance(NnId const x, NnId const y) -> usize {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE);
        return DISTANCE[x][y];
    }

    FORCE_INLINE
    static auto set_distance(NnId const x, NnId const y, usize const distance) -> void {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE);
        DISTANCE[x][y] = distance;
    }

    /// The node |cpu| works on now; NUMA_MAX_NODE while nothing has claimed
    /// it. Starts as firmware_node() and follows set_cpu_node() after that.
    FORCE_INLINE
    static auto cpu_to_node(CpuNum const cpu) -> NnId {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        return CPU_TO_NODE[cpu];
    }

    /// The node the calling CPU belongs to.
    ///
    /// Read out of the table rather than through PmNode::Current, and not as
    /// an optimisation. Current is a per-CPU `PmNode *`, so
    /// `CURRENT_INSTANCE->m_id` dereferences the *template* copy of that
    /// pointer at compile time -- it folds to `&(nullptr->m_id)`, i.e. the
    /// constant 4, which then gets handed to %gs as if it were a per-CPU
    /// handle. That is a wild access whichever model is in effect, and it
    /// stays wrong after the pointer is set, because the fold already
    /// happened.
    ///
    /// The table is also the only answer that works this early: nothing fills
    /// the per-CPU pointer until PmNode::Current::init() runs from an init
    /// hook, while allocation starts asking for a node long before that.
    FORCE_INLINE
    static auto current_node() -> NnId {
        return cpu_to_node(CpuLocal::current_cpu());
    }

    /// The node the firmware put |cpu| on -- what a node's hard affinity is
    /// built from. Unlike cpu_to_node(), a run-time move does not change it.
    FORCE_INLINE
    static auto firmware_node(CpuNum const cpu) -> NnId {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        return FIRMWARE_CPU_TO_NODE[cpu];
    }

    /// Records the firmware's placement of |cpu|, which is also where it
    /// starts out. Only while bringing up: the SRAT parse, and the fallback
    /// claims in PmNode::bootstrap() and PmNode::Current::claim_possible_cpus().
    FORCE_INLINE
    static auto set_cpu_node_early(CpuNum const cpu, NnId const nid) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        FIRMWARE_CPU_TO_NODE[cpu] = nid;
        CPU_TO_NODE[cpu] = nid;
    }

    /// Moves |cpu| to |nid| in the table. Only the table: the nodes' own CPU
    /// masks are PmNode::Current::set_nid()'s to update, which calls this.
    FORCE_INLINE
    static auto set_cpu_node(CpuNum const cpu, NnId const nid) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        CPU_TO_NODE[cpu] = nid;
    }

  private:
    static std::array<std::array<usize, NUMA_MAX_NODE>, NUMA_MAX_NODE> DISTANCE;

    /// Both start out on NUMA_MAX_NODE, the "no node known yet" sentinel;
    /// see pm_node_topology.cpp.
    static std::array<NnId, MAX_CPU> FIRMWARE_CPU_TO_NODE;
    static std::array<NnId, MAX_CPU> CPU_TO_NODE;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_NUMA_TOPOLOGY_HPP
