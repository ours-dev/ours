/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/pm_node.hpp>

#include <algorithm>

#include <numa/topology.hpp>

namespace ours::mm {
/// MAX_NODES is the "not placed yet" sentinel, and constinit keeps it there
/// from the first instruction: current_node() reads this table before any init
/// hook could have filled it.
constinit decltype(CpuNodeMap::NODE_OF_CPU) CpuNodeMap::NODE_OF_CPU = [] {
    decltype(CpuNodeMap::NODE_OF_CPU) value;
    std::fill(std::begin(value), std::end(value), NodeId(MAX_NODES));
    return value;
}();

auto CpuNodeMap::seed_from_firmware(NodeId const fallback, NodeMask const &nodes) -> usize {
    DEBUG_ASSERT(fallback < MAX_NODES, "the fallback node does not exist");
    DEBUG_ASSERT(nodes.test(fallback), "the fallback node is not one that exists");

    // The firmware's answer where it names a node that exists, the fallback
    // otherwise. This is the only place the two tables meet: afterwards the
    // running placement is mm's and a move writes only it, while the
    // firmware's stays what the firmware said -- which is what a node's hard
    // affinity is built from, and so must not follow a move.
    //
    // "Exists" is |nodes|, not merely "below MAX_NODES", which is all this
    // used to check. A proximity domain with CPUs and no memory passes that
    // test and has no PmNode behind it, so its CPUs were placed on a node that
    // was never created, and the first publish_cpu() for one went through the
    // null: an assertion panic on the debug image, while the image that
    // compiles assertions out wrote through the null pointer -- and cached it
    // as that CPU's local node -- and booted on with no symptom at all.
    usize refused = 0;
    auto const seed = [fallback, &nodes, &refused](CpuNum const cpu) {
        auto const declared = numa::Topology::firmware_node(cpu);
        if (declared < MAX_NODES && nodes.test(declared)) {
            set_node_of(cpu, declared);
            return;
        }
        if (declared < MAX_NODES) {
            refused += 1;
        }
        set_node_of(cpu, fallback);
    };

    // The possible mask, and the CPU this is running on by name. The mask is
    // empty the first time this is called unless an SRAT parse has filled it,
    // because the MADT parse runs several init levels after init_pmm(); and a
    // machine that handed us no MADT still has the CPU executing this, and
    // leaving that one unplaced is the failure the whole seed exists to
    // prevent -- current_node() would answer MAX_NODES, and its callers index
    // MAX_NODES-long arrays with it. One walk over the union, so that no CPU
    // is counted twice.
    //
    // Idempotent, so the later call once the MADT is parsed simply places the
    // CPUs that were not in the mask yet.
    auto cpus = Cpu::possible_mask();
    cpus.set(CpuLocal::current_cpu());
    cpus.for_each(seed);
    return refused;
}

auto CpuNodeMap::local_node() -> PmNode * {
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

auto CpuNodeMap::cache_local_node(CpuNum const cpu, PmNode *const node) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    DEBUG_ASSERT(node, "the cache holds a node or stays a miss, never null");
    DEBUG_ASSERT(node->nid() == node_of(cpu),
                 "CPU {} is placed on node {}, so its cache cannot hold node {}", cpu, node_of(cpu),
                 node->nid());
    Slot::set(node, cpu);
}

} // namespace ours::mm
