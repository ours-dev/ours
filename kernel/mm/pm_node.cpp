#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/panic.hpp>

#include <algorithm>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <numa/topology.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/util/enum_sequence.hpp>
namespace ours::mm {
/// Chains the zones of every node in |order| behind this node's own, in the
/// order given.
///
/// m_connected records which nodes were chained in. Each remote node
/// contributes its own zones in type order -- the list LocalContiguous used to
/// keep a second copy of.
///
/// It used to take a NodeMask and call Router::build() to turn it into an
/// order, which meant this function read the machine's distance matrix. The
/// derivation is numa::build_fallback_orders(), asked once by bring-up; what
/// arrives here is already a list of node ids, and the only thing left to
/// decide is nothing.
auto ZoneList::connect(ustl::views::Span<NodeId const> const order) -> void {
    for (auto const next: order) {
        DEBUG_ASSERT(next != m_nid, "node {} was given itself as a fallback hop", m_nid);
        PmNode::node(next)->zone_list()->for_each_local_zone(
            [this](PmZone &zone) { push_fallback(&zone); });
        m_connected.set(next);
    }
}

/// Visits the zones in allocation order: the fallback list already *is* that
/// order, this node's own zones first and the remote ones in the order
/// connect() computed.
///
/// A callback returning void visits every zone. One returning a value stops at
/// the first result that tests true and yields it -- which is what makes this
/// usable as "take a frame from the first zone that can spare one" rather than
/// only as a traversal.
template <typename F>
auto ZoneList::for_each_zone(F &&f) -> ustl::traits::InvokeResultT<F, PmZone &> {
    using Result = ustl::traits::InvokeResultT<F, PmZone &>;

    if constexpr (std::is_void_v<Result>) {
        for (auto *const zone: m_fallback) {
            f(*zone);
        }
    } else {
        for (auto *const zone: m_fallback) {
            if (auto result = f(*zone)) {
                return result;
            }
        }
        return Result{};
    }
}

template <typename F, typename Predicate>
auto ZoneList::for_each_zone_if(F &&f, Predicate &&p) -> ustl::traits::InvokeResultT<F, PmZone &> {
    return for_each_zone([&](PmZone &zone) { return p(zone) ? f(zone) : decltype(f(zone)){}; });
}

auto ZoneList::dump_fallback() const -> void {
    log::debug("Node[{}-fallback]", m_nid);
    for (auto const *const zone: m_fallback) {
        log::debug("    None-{}| ZoneType-{:<6}| [0x{:X}, 0x{:X})", zone->which_node(),
                   to_string(zone->zone_type()), zone->start_pfn(), zone->end_pfn());
    }
}

decltype(PmNode::STATES) PmNode::STATES;
decltype(PmNode::NODES) PmNode::NODES;
decltype(PmNode::NUM_NODES) PmNode::NUM_NODES;

PmNode::PmNode(NodeId nid)
    : m_canary(this)
    , m_id(nid)
    , m_lock()
    , m_hard_affinity()
    , m_soft_affinity()
    , m_start_pfn()
    , m_zone_list(nid)
    , m_page_queues() {}

auto PmNode::alloc_frames(AllocRequest const &request) -> ustl::Result<PmFrame *, BlockFailure> {
    m_canary.verify<Self>();
    DEBUG_ASSERT(request.home_node() == m_id, "node {} was handed a request whose home is node {}",
                 m_id, request.home_node());

    ustl::Option<PmZone const &> depleted_zone;
    for (auto &zone: m_zone_list.candidates(request)) {
        auto const block = zone.alloc_frames(request.order());
        if (!block) {
            if (request.order() == 0 && !depleted_zone &&
                zone.num_free_frames() < zone.watermark(PmZone::Watermark::High)) {
                depleted_zone = ustl::some_ref(zone);
            }
            continue;
        }

        return ustl::ok(&block.value());
    }

    return ustl::err(BlockFailure{.status = Status::OutOfMem, .depleted_zone = depleted_zone});
}

auto PmNode::free_frames(PmFrame &frame, usize const order) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(frame.nid() == m_id, "frame {:#x} of node {} was returned to node {}",
                 frame_to_phys(&frame), frame.nid(), m_id);

    auto *const zone = m_zone_list.local_zone(frame.zone());
    DEBUG_ASSERT(zone, "frame {:#x} is in no zone", frame_to_phys(&frame));

    zone->free_frames(frame, order);
}

auto PmNode::contains(usize const order, ZoneType const type) -> bool {
    m_canary.verify<Self>();

    auto *const zone = m_zone_list.local_zone(type);
    if (!zone) {
        return false;
    }
    return zone->contains(order);
}

auto PmNode::dump() const -> void {
    // TODO(SmallHuaZi) merge them to a log sentence.
    // The node's own present and reserved counts went: they were the sum of
    // its zones' and only this function read them. The per-zone lines below
    // carry the same information, once each.
    log::info(
        R"(Node[{}]: 
                start pfn: {}
                spanned frames: {}
            )",
        m_id, m_start_pfn, m_spanned_frames.load());

    m_zone_list.for_each_local_zone([](PmZone &zone) {
        log::info(R"(ZoneType[{}]: 
                    start pfn: {}
                    spanned frames: {}
                    present frames: {}
                    reserved frames: {}
                )",
                  to_string(zone.zone_type()), zone.start_pfn(), zone.spanned_frames(),
                  zone.present_frames(), zone.reserved_frames());
    });
}

auto PmNode::set_cpu_node(CpuNum const cpu, NodeId const new_nid) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU);
    DEBUG_ASSERT(new_nid < MAX_NODES);

    auto const old_nid = CpuNodeMap::node_of(cpu);
    if (old_nid == new_nid) {
        return;
    }

    if (old_nid != MAX_NODES) {
        if (auto node = PmNode::node(old_nid)) {
            node->m_canary.verify<Self>();
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};

            // Erases ourselves from the node to which the CPU belongs.
            node->m_work_cpus.set(cpu, 0);
        }
    }

    // The table before the cache, because the cache asserts it agrees with the
    // table. Neither node's lock is held across both: the two masks are
    // separate invariants, and a CPU appearing in neither for an instant is
    // what a reader of work_cpus() already has to tolerate -- it takes a copy.
    CpuNodeMap::set_node_of(cpu, new_nid);
    if (auto node = PmNode::node(new_nid)) {
        node->m_canary.verify<Self>();
        {
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
            node->m_work_cpus.set(cpu);
        }
        CpuNodeMap::cache_local_node(cpu, node);
    }
}

auto PmNode::publish_cpu(CpuNum const cpu) -> void {
    auto const nid = CpuNodeMap::node_of(cpu);
    if (nid == MAX_NODES) {
        // Not claimed yet. bootstrap() claims only the CPU it runs on;
        // claim_possible_cpus() and the NUMA topology parse claim the rest.
        return;
    }

    auto *const node = PmNode::node(nid);
    DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist", cpu, nid);
    node->m_canary.verify<Self>();

    {
        Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
        node->m_work_cpus.set(cpu);
        // PmNode::init() builds the hard affinity from
        // numa::Topology::firmware_node(), but only for the CPUs claimed by
        // then -- the boot CPU. A CPU claimed later joins it here, so that the
        // node's mask says the same thing as the table.
        if (numa::Topology::firmware_node(cpu) == nid) {
            node->m_hard_affinity.set(cpu);
        }
    }
    CpuNodeMap::cache_local_node(cpu, node);
}

/// Publishes each CPU's node into its per-CPU slot, now that the slots exist.
///
/// Not through set_cpu_node(). bootstrap() has already claimed every CPU with
/// CpuNodeMap::seed_from_firmware(), so set_cpu_node() finds the old and
/// new ids equal and returns before it reaches the cache. That is what left
/// the slot null across the zero-page allocation in init_vmm_preheap(): the
/// node-less alloc_frames() went on through a null PmNode, and
/// The allocation walk spun on the ZoneList it read out of address
/// 0x48. The early return is right for what set_cpu_node() is for -- a CPU
/// moving between nodes at run time, where nothing changing means nothing to
/// do -- but the first publish is this function's job, and it has to be
/// unconditional.
///
/// The running CPU is covered by name, the way bootstrap() covers it. The
/// possible mask is filled by the MADT parse, which runs from
/// platform_init_apic() at InitLevel::Irq -- three levels after this hook --
/// so at this point it is empty, and a walk of it alone publishes nothing.
/// That was the other way this used to fail on the same boot.
auto PmNode::publish_cpus() -> void {
    Cpu::possible_mask().for_each(&PmNode::publish_cpu);
    publish_cpu(CpuLocal::current_cpu());
}

/// Every CPU the MADT reported gets a node -- before any of them runs.
///
/// bootstrap() could only claim the CPU it runs on: the possible mask is
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
/// first node that owns memory -- the fallback bootstrap() chose, and on a
/// machine without an SRAT the only node there is.
auto PmNode::claim_possible_cpus() -> void {
    auto const fallback = current_node();
    DEBUG_ASSERT(fallback < MAX_NODES, "the boot CPU has no node");

    // The seed is idempotent: a CPU the firmware placed keeps that node, and
    // one it did not lands on |fallback|. Running it again once the MADT has
    // been parsed is how the CPUs that were not in the possible mask during
    // init_pmm() get placed at all.
    // The count is not reported again: bootstrap() already said it for every
    // CPU the firmware bound, and the MADT adds only CPUs the firmware did not.
    static_cast<void>(CpuNodeMap::seed_from_firmware(fallback, node_possible_mask()));
    Cpu::possible_mask().for_each(&PmNode::publish_cpu);
}

/// Once per-CPU storage exists, so that the node-less alloc_frames() can be
/// called at all. CpuLocal + 1 rather than CpuLocal itself: the hooks at a
/// level run when that level is announced, and CpuLocal is announced right
/// after CpuLocal::init() returns -- a hook at exactly that level would run
/// with the units laid out but before main.cpp had said so.
GKTL_INIT_HOOK(PublishCpus, PmNode::publish_cpus, gktl::InitLevel::CpuLocal + 1);

/// After InitLevel::Irq, where the MADT parse fills the possible mask, and
/// before InitLevel::Platform, where x86_bring_aps() starts the CPUs in it.
GKTL_INIT_HOOK(ClaimPossibleCpus, PmNode::claim_possible_cpus, gktl::InitLevel::Irq + 1);

} // namespace ours::mm
