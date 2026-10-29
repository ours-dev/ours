/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/node_placement.hpp>
#include <ours/mm/pm_node.hpp>

#include <algorithm>

#include <numa/topology.hpp>

namespace ours::mm {
/// MAX_NODES is the "not placed yet" sentinel, and constinit keeps it there
/// from the first instruction: current_node() reads this table before any init
/// hook could have filled it.
constinit decltype(NodePlacement::NODE_OF_CPU) NodePlacement::NODE_OF_CPU = [] {
    decltype(NodePlacement::NODE_OF_CPU) value;
    std::fill(std::begin(value), std::end(value), NodeId(MAX_NODES));
    return value;
}();

auto NodePlacement::seed_from_firmware(NodeId const fallback) -> void {
    DEBUG_ASSERT(fallback < MAX_NODES, "the fallback node does not exist");

    // The firmware's answer where there is one, the fallback where there is
    // not. This is the only place the two tables meet: afterwards the running
    // placement is mm's and a move writes only it, while the firmware's stays
    // what the firmware said -- which is what a node's hard affinity is built
    // from, and so must not follow a move.
    auto const seed = [fallback](CpuNum const cpu) {
        auto const declared = numa::Topology::firmware_node(cpu);
        if (declared < MAX_NODES) {
            set_node_of(cpu, declared);
        } else {
            set_node_of(cpu, fallback);
        }
    };

    Cpu::possible_mask().for_each(seed);

    // And the CPU this is running on, explicitly, because the possible mask is
    // empty the first time this is called: the MADT parse that fills it runs
    // several init levels after init_pmm(). A machine that handed us no MADT
    // still has the CPU executing this, and leaving that one unplaced is the
    // failure the whole seed exists to prevent -- current_node() would answer
    // MAX_NODES, and its callers index MAX_NODES-long arrays with it.
    //
    // Idempotent, so the later call once the MADT is parsed simply places the
    // CPUs that were not in the mask yet.
    seed(CpuLocal::current_cpu());
}

auto NodePlacement::local_node() -> PmNode * {
    // The cache, and the only answer needed once the publish hook has run.
    if (auto *const node = Slot::get()) {
        return node;
    }

    // The miss: recompute it from the table the cache is a copy of. node_of()
    // rather than current_node(), which is this same expression wearing mm's
    // public name -- going through it would make the node layer depend on the
    // allocator's front door to answer a question it owns.
    auto const nid = node_of(CpuLocal::current_cpu());
    if (nid >= MAX_NODES) {
        return nullptr;
    }
    return PmNode::node(nid);
}

auto NodePlacement::cache_local_node(CpuNum const cpu, PmNode *const node) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    DEBUG_ASSERT(node, "the cache holds a node or stays a miss, never null");
    DEBUG_ASSERT(node->nid() == node_of(cpu),
                 "CPU {} is placed on node {}, so its cache cannot hold node {}", cpu, node_of(cpu),
                 node->nid());
    Slot::set(node, cpu);
}

} // namespace ours::mm
