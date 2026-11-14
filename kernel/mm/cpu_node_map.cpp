/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/pm_node.hpp>

#include <algorithm>

#include <gktl/init_hook.hpp>
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
    // was never created, and the first publish() for one went through the
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
    return find_node(nid);
}

auto CpuNodeMap::cache_local_node(CpuNum const cpu, PmNode *const node) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    DEBUG_ASSERT(node, "the cache holds a node or stays a miss, never null");
    DEBUG_ASSERT(node->nid() == node_of(cpu),
                 "CPU {} is placed on node {}, so its cache cannot hold node {}", cpu, node_of(cpu),
                 node->nid());
    Slot::set(node, cpu);
}

auto CpuNodeMap::publish(CpuNum const cpu) -> void {
    auto const nid = node_of(cpu);
    if (nid == MAX_NODES) {
        // Not claimed yet. Node bring-up claims only the CPU it runs on;
        // claim_possible() and the NUMA topology parse claim the rest.
        return;
    }

    auto *const node = find_node(nid);
    DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist", cpu, nid);

    node->add_cpu(cpu);
    // The node's init() builds the hard affinity from
    // numa::Topology::firmware_node() when the node is made. A CPU published
    // here joins it the same way, so that the node's mask says the same thing
    // as the firmware's table whenever the CPU was claimed.
    node->join_hard_affinity(cpu);
    cache_local_node(cpu, node);
}

/// Publishes each CPU's node into its per-CPU slot, now that the slots exist.
///
/// Not through move(). Node bring-up has already claimed every CPU with
/// seed_from_firmware(), so move() finds the old and new ids equal and
/// returns before it reaches the cache. That is what left the slot null
/// across the zero-page allocation in init_vmm_preheap(): the allocation with
/// no home went on through a null PmNode, and the walk spun on the ZoneList
/// it read out of address 0x48. The early return is right for what move() is
/// for -- a CPU moving between nodes at run time, where nothing changing
/// means nothing to do -- but the first publish is this function's job, and
/// it has to be unconditional.
///
/// The running CPU is covered by name, the way node bring-up covers it. The
/// possible mask is filled by the MADT parse, which runs from
/// platform_init_apic() at InitLevel::Irq -- three levels after this hook --
/// so at this point it is empty, and a walk of it alone publishes nothing.
/// That was the other way this used to fail on the same boot.
auto CpuNodeMap::publish_all() -> void {
    Cpu::possible_mask().for_each(&CpuNodeMap::publish);
    publish(CpuLocal::current_cpu());
}

/// Every CPU the MADT reported gets a node -- before any of them runs.
///
/// Node bring-up could only claim the CPU it runs on: the possible mask is
/// filled by the MADT parse (platform_init_apic(), InitLevel::Irq), long
/// after the PMM. Every other CPU was left on the MAX_NODES sentinel, and
/// nothing ever moved it -- the only other claimant is the SRAT parse, which
/// a kernel built without NUMA never compiles in and a machine without an
/// SRAT never feeds. So current_node() on an AP answered MAX_NODES, and
/// the first kmalloc() on an AP -- a user thread's channel_create(), on
/// CPU1 -- read m_node_caches[MAX_NODES], one past the end, took the
/// ListMemberHook after the array for a NodeCache, and spun for ever on a list
/// pointer it had mistaken for that NodeCache's lock. The comment on
/// claim_unplaced_cpus() in pm_node_init.cpp describes that exact failure for
/// the boot CPU; this is the same hole for the others.
///
/// A CPU no topology source placed goes where the boot CPU went, which is the
/// first node that owns memory -- the fallback node bring-up chose, and on a
/// machine without an SRAT the only node there is.
auto CpuNodeMap::claim_possible() -> void {
    auto const fallback = node_of(CpuLocal::current_cpu());
    DEBUG_ASSERT(fallback < MAX_NODES, "the boot CPU has no node");

    // The seed is idempotent: a CPU the firmware placed keeps that node, and
    // one it did not lands on |fallback|. Running it again once the MADT has
    // been parsed is how the CPUs that were not in the possible mask during
    // init_pmm() get placed at all.
    //
    // The count is dropped on purpose: node bring-up already said it for
    // every CPU the firmware bound, and the MADT adds only CPUs the firmware
    // did not.
    auto _ = seed_from_firmware(fallback, node_possible_mask());
    Cpu::possible_mask().for_each(&CpuNodeMap::publish);
}

auto CpuNodeMap::move(CpuNum const cpu, NodeId const new_nid) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU);
    DEBUG_ASSERT(new_nid < MAX_NODES);

    auto const old_nid = node_of(cpu);
    if (old_nid == new_nid) {
        return;
    }

    if (old_nid != MAX_NODES) {
        if (auto *const node = find_node(old_nid)) {
            node->remove_cpu(cpu);
        }
    }

    // The table before the cache, because the cache asserts it agrees with the
    // table. Neither node's lock is held across both: the two masks are
    // separate invariants, and a CPU appearing in neither for an instant is
    // what a reader of PmNode::work_cpus() already has to tolerate -- it
    // takes a copy.
    set_node_of(cpu, new_nid);
    if (auto *const node = find_node(new_nid)) {
        node->add_cpu(cpu);
        cache_local_node(cpu, node);
    }
}

/// Once per-CPU storage exists, so that an allocation with no home can be
/// made at all. main.cpp announces CpuLocal right after CpuLocal::init()
/// returns, and that one call runs every hook from CpuLocal up to the next
/// level, before the level it records changes: a hook at CpuLocal and one at
/// CpuLocal + 1 run in the same call. The + 1 only orders this one after any
/// hook registered at exactly CpuLocal.
GKTL_INIT_HOOK(PublishCpus, CpuNodeMap::publish_all, gktl::InitLevel::CpuLocal + 1);

/// After InitLevel::Irq, where the MADT parse fills the possible mask, and
/// before InitLevel::Platform, where x86_bring_aps() starts the CPUs in it.
GKTL_INIT_HOOK(ClaimPossibleCpus, CpuNodeMap::claim_possible, gktl::InitLevel::Irq + 1);

} // namespace ours::mm
