/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Which node each CPU is working on.
///
/// The node layer's own table, not the firmware's. kernel/lib/numa holds what
/// the firmware said -- a distance matrix and the placement it declared -- and
/// this is where that lands once, at bring-up, plus whatever a run-time move
/// has done to it since. So the allocator asks a question in its own terms,
/// "which node is this CPU on", and never asks anything about NUMA on a path
/// an allocation takes.
///
/// Why a table of mm's own rather than a call into kernel/lib/numa each time:
/// mm has to answer this before any of its per-CPU state exists. PmNode's
/// per-CPU pointer is not filled until an init hook runs, while allocation
/// starts asking for a node long before that, so the answer has to come from
/// somewhere that is correct from the first instruction. That was decision
/// D2(a) of .agents/tasks/numa-out-of-mm/DESIGN.md.
#ifndef OURS_MM_NODE_PLACEMENT_HPP
#define OURS_MM_NODE_PLACEMENT_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours::mm {
/// The running placement: one node id per CPU.
///
/// Every accessor is static because there is one machine, and the table is
/// private so that the bounds check is the only way in.
class NodePlacement {
    using Self = NodePlacement;

  public:
    /// The node |cpu| is working on, or MAX_NODES while nothing has placed it.
    ///
    /// MAX_NODES doubles as the "not placed yet" sentinel, which is how the
    /// table it replaced read too. It is not INVALID_NODE_ID, and the two
    /// existing side by side is filed as a thing to settle --
    /// docs/changes/261028/mm.md, "The node vocabulary stops saying NUMA".
    FORCE_INLINE
    static auto node_of(CpuNum const cpu) -> NodeId {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        return NODE_OF_CPU[cpu];
    }

    /// Places |cpu| on |nid|. Both the seed at bring-up and a run-time move
    /// come through here; the nodes' own CPU masks belong to PmNode, which
    /// updates them around this call.
    FORCE_INLINE
    static auto set_node_of(CpuNum const cpu, NodeId const nid) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        NODE_OF_CPU[cpu] = nid;
    }

    /// Seeds the table from what the firmware declared, and places anything
    /// the firmware did not on |fallback|.
    ///
    /// Called once, from PmNode's bring-up, after whatever reads the firmware
    /// has run and before the first allocation that cares. A machine with no
    /// NUMA source leaves every entry unplaced, so every CPU lands on
    /// |fallback|, which is the single node such a machine has.
    static auto seed_from_firmware(NodeId fallback) -> void;

  private:
    /// MAX_NODES from the first instruction, by constinit, because this is
    /// read before any init hook could fill it.
    static std::array<NodeId, MAX_CPU> NODE_OF_CPU;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_NODE_PLACEMENT_HPP
