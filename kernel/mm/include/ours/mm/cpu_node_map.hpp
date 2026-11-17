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
#ifndef OURS_MM_CPU_NODE_MAP_HPP
#define OURS_MM_CPU_NODE_MAP_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

#include <array>

namespace ours::mm {
class PmNode;

/// The running placement: one node id per CPU, plus the pointer that id
/// resolves to, cached per CPU.
///
/// Every accessor is static because there is one machine, and the state is
/// private so that the bounds check and the cache's invariant are the only way
/// in.
///
/// The table is the truth and the pointer is strictly a function of it --
/// find_node(node_of(cpu)) -- so it is a cache and is treated as one:
/// local_node() falls back to recomputing it, and a null slot is a miss rather
/// than a fault. That is the opposite arrangement from the firmware placement
/// in kernel/lib/numa, which is a second independent fact and so must never be
/// written from here.
///
/// The base is where the per-CPU slot lives, and it is inherited privately on
/// purpose: its get()/set() invite `CpuNodeMap::get()->m_id`, which reads
/// CURRENT_INSTANCE as an ordinary pointer -- the fold-to-a-constant trap
/// described on current_node() -- and the private base keeps them out of this
/// class's interface. It cannot keep them out of reach altogether.
/// CpuLocal::Current's storage belongs to the instantiation, not to the class
/// that derives it, so anything that names CpuLocal::Current<PmNode *> itself
/// reaches this one slot; a slot type of its own would cost the one-instruction
/// read the scalar form gives, so the rule is that nothing else names it.
///
/// The writers of the table and the cache are private for the same reason, and
/// the nodes' CPU bookkeeping takes a CpuPlacementKey that only this class
/// (and a node building its own hard affinity) can make: one class keeps the
/// table, the masks and the cache in step.
class CpuNodeMap: private CpuLocal::Current<PmNode *> {
    using Self = CpuNodeMap;
    using Slot = CpuLocal::Current<PmNode *>;

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

    /// Seeds the table from what the firmware declared, and places anything
    /// the firmware did not on |fallback|.
    ///
    /// Called from PmNode's bring-up, after whatever reads the firmware has
    /// run and before the first allocation that cares, and again once the
    /// MADT has been parsed. A machine with no NUMA source leaves every entry
    /// unplaced, so every CPU lands on |fallback|, which is the single node
    /// such a machine has.
    ///
    /// |nodes| is the set of nodes that exist: the ones that own memory, and
    /// so get a PmNode. The firmware places CPUs on proximity domains whether
    /// or not those own memory -- a socket with no DIMMs is an ordinary
    /// machine -- and a CPU whose declared node is not in |nodes| lands on
    /// |fallback| as though the firmware had said nothing. Returns how many
    /// CPUs that happened to, so bring-up can say so.
    static auto seed_from_firmware(NodeId fallback, NodeMask const &nodes) -> usize;

    /// The calling CPU's node, answered even before the cache exists.
    ///
    /// The cached pointer when there is one, and otherwise the table and the
    /// node it names. The slot is filled by publish_all(), an init
    /// hook at InitLevel::CpuLocal + 1 -- after CpuLocal::init(), which
    /// allocates the per-CPU units themselves and the page tables that map
    /// them. Read before that, the slot answers null, and a null PmNode is not
    /// a failure the allocation paths notice on their own: they walk straight
    /// into it and read a ZoneList out of low memory, which phys's identity map
    /// still makes readable, so the wreck surfaces somewhere else entirely.
    /// That has happened twice -- once on init_vmm_preheap()'s zero-page
    /// allocation, once as a #GP inside CpuLocal::init() -- which is why the
    /// miss path lives here rather than in each caller.
    ///
    /// Null means no node has claimed this CPU at all, which is a caller error
    /// rather than a shortage: bootstrap() claims the CPU it runs on before the
    /// first allocation, and claim_possible() the rest before any of them
    /// starts.
    static auto local_node() -> PmNode *;

    /// Makes |cpu|'s place in the table known to the node and to the cache:
    /// records it as working on the node the table puts it on, and points its
    /// slot at that node. Nothing for a CPU still on the MAX_NODES sentinel.
    static auto publish(CpuNum cpu) -> void;

    /// publish() for every CPU claimed so far. Run once, from an init hook,
    /// as soon as per-CPU storage exists; see the definition for why it is
    /// unconditional rather than going through move().
    static auto publish_all() -> void;

    /// Gives every CPU the firmware reported a node, and publishes it, before
    /// any of them starts. Run once, from an init hook after the MADT parse;
    /// see the definition for why node bring-up could not do this itself.
    static auto claim_possible() -> void;

    /// Moves |cpu| to node |nid|: out of its old node's CPU mask, into the new
    /// one's, and the table and its cache along with them.
    ///
    /// No caller yet. It is kept because it is the one place that knows what a
    /// move has to touch, and it is three objects wide -- two nodes' masks,
    /// each under its own lock, and the table -- so the next thing that
    /// re-homes a CPU should not have to rediscover the order.
    static auto move(CpuNum cpu, NodeId nid) -> void;

  private:
    /// Places |cpu| on |nid| in the table, and nothing else. Both the seed at
    /// bring-up and move() come through here; the nodes' own CPU masks are
    /// each node's, and publish() and move() update them around this call.
    FORCE_INLINE
    static auto set_node_of(CpuNum const cpu, NodeId const nid) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        NODE_OF_CPU[cpu] = nid;
    }

    /// Points |cpu|'s slot at |node|, which must be the node the table places
    /// it on -- the cache is only ever a copy of that composition.
    ///
    /// Only after CpuLocal::init(): this writes through the per-CPU base, and
    /// there is nothing to ask whether the units exist yet. Every caller is an
    /// init hook at InitLevel::CpuLocal + 1 or later, or a run-time move.
    static auto cache_local_node(CpuNum cpu, PmNode *node) -> void;

    /// MAX_NODES from the first instruction, by constinit, because this is
    /// read before any init hook could fill it.
    static std::array<NodeId, MAX_CPU> NODE_OF_CPU;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_CPU_NODE_MAP_HPP
