/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Bringing the nodes up: everything PmNode does once, at boot, before the
/// first frame is allocated. Kept apart from pm_node.cpp, which is what runs
/// for the life of the kernel, the way memmap_init.cpp is kept apart from
/// MemMap.
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>

#include <algorithm>
#include <utility>

#include <logz4/log.hpp>
#include <numa/fallback_order.hpp>
#include <numa/topology.hpp>
#include <ustl/bit.hpp>

namespace ours::mm {
namespace {
    /// Marks Possible every node that owns a usable memory range.
    auto find_possible_nodes() -> Status {
        return EarlyMem::visit_usable(EarlyMem::min_address(), EarlyMem::max_address(),
                                      bootmem::ANY_NODE, [](bootmem::MemoryRegion const region) {
                                          set_node_state(region.node, NodeState::Possible);
                                          return true;
                                      });
    }

    /// The first node in |nodes|, or MAX_NODES when there is none.
    auto first_node(NodeMask const &nodes) -> NodeId {
        auto first = NodeId(MAX_NODES);
        nodes.for_each([&first](NodeId const nid) {
            if (first == MAX_NODES) {
                first = nid;
            }
        });
        return first;
    }

    /// The whole fallback table, asked of kernel/lib/numa once and reused for
    /// every node.
    ///
    /// File scope rather than a local, because on a NUMA build it is MAX_NODES
    /// rows of MAX_NODES - 1 node ids -- about four kilobytes -- and this runs
    /// on a bring-up kernel stack. Dead the moment connect_all() returns, so
    /// it costs .bss on a NUMA build and nothing at all on a build where
    /// MAX_NODES is 1, where each row has capacity zero.
    numa::FallbackOrders FALLBACK_ORDERS;

    /// floor(sqrt(value)), one bit of the answer at a time, highest first.
    ///
    /// Here because the machine-wide reserve grows with the square root of
    /// memory rather than in proportion to it. A fixed fraction that is right
    /// on a small machine holds back gigabytes on a large one, and how much
    /// has to stay free for the kernel to make progress simply does not grow
    /// linearly with how much there is.
    auto isqrt(usize value) -> usize {
        if (value <= 1) {
            return value;
        }

        // The highest even bit position at or below the top set bit: the
        // answer's top bit cannot sit higher than half of that.
        auto bit = usize{1} << ((ustl::bit_width(value) - 1) & ~usize{1});
        auto result = usize{0};
        while (bit != 0) {
            auto const probe = result + bit;
            result >>= 1;
            if (value >= probe) {
                value -= probe;
                result += bit;
            }
            bit >>= 2;
        }
        return result;
    }

    /// How many frames the whole machine holds back from allocation, which the
    /// zones then divide between them as their Min watermarks.
    ///
    /// Four times the square root of the memory in KiB, clamped at both ends,
    /// converted back into frames. The clamps are what make the curve usable
    /// rather than merely monotonic: without the lower one a small machine
    /// reserves so little that the kernel cannot allocate its way out of
    /// trouble, and without the upper one a very large machine reserves
    /// hundreds of megabytes it will never need at once.
    auto machine_reserve_frames(usize const managed_total) -> usize {
        constexpr auto KIB_PER_FRAME = usize(PAGE_SIZE / 1024);
        constexpr auto MIN_RESERVE_KIB = usize(128);
        constexpr auto MAX_RESERVE_KIB = usize(256 * 1024);

        auto const managed_kib = managed_total * KIB_PER_FRAME;
        auto const reserve_kib =
            std::clamp(isqrt(managed_kib * 16), MIN_RESERVE_KIB, MAX_RESERVE_KIB);
        return reserve_kib / KIB_PER_FRAME;
    }

    auto dump_fallback_queues(NodeMask const &nodes) -> void {
        nodes.for_each([](NodeId const nid) { find_node(nid)->zone_list().dump_fallback(); });
    }
} // namespace

/// Chains every node's zones into its fallback order. A member of PmNode so
/// that it, and nothing after bring-up, can reach a node's list to build it. Needs every node to
/// exist and be initialised first, for two reasons: connect() walks the
/// other nodes' ZoneList, and the members the order is derived over are
/// the nodes that own memory, which is a state PmNode::init() sets.
///
/// One call for the whole machine rather than one per node. The derivation
/// is the same scan either way; asking once is what keeps the distance
/// matrix to a single reader, in one place, at one moment in bring-up.
auto PmNode::connect_all(NodeMask const &nodes) -> void {
    // Every node gets a row, but only the ones that own memory can be a
    // hop in it. The two sets were one argument before -- connect() took
    // the candidate mask and the derivation walked the Memory state inside
    // it -- and collapsing them here would have quietly left a
    // memory-less node with no fallback order at all, which is the one
    // node that cannot do without one.
    auto const members = nodes & node_memory_mask();
    numa::build_fallback_orders(nodes, members, &FALLBACK_ORDERS);

    nodes.for_each([](NodeId const nid) {
        auto const &order = FALLBACK_ORDERS[nid];
        find_node(nid)->m_zone_list.connect(
            ustl::views::Span<NodeId const>{order.data(), order.size()});
    });
}

/// Brings up every node, in the order each step depends on: which node is
/// which and which CPUs are on it, then every zone, then the nodes, each
/// taking its own zones, then the fallback order that chains them.
auto PmNode::bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status {
    ASSERT(!max_zone_pfn.empty());

    if (auto const status = find_possible_nodes(); Status::Ok != status) {
        return status;
    }

    auto const &nodes = node_possible_mask();
    auto const fallback = first_node(nodes);
    if (fallback == MAX_NODES) {
        // No node owns usable memory; the steps below would build nothing and
        // every allocation would fail somewhere less obvious than here.
        return Status::BadState;
    }
    // Every CPU that no topology source claimed goes to the node |fallback|.
    //
    // The firmware's CPU-to-node table starts out filled with MAX_NODES
    // -- the "no node known yet" sentinel -- and the only thing that ever
    // overwrites it is a topology source: on x86 the SRAT parse, which a
    // kernel built without NUMA never compiles in and a machine without an
    // SRAT never feeds. Left on the sentinel, that is what current_node()
    // returns, and its callers index MAX_NODES-long arrays with it --
    // MemCache::allocate() reads m_node_caches[MAX_NODES], one past the
    // end, takes the ListMemberHook that follows the array for a NodeCache,
    // and spins forever on a list pointer it has mistaken for that
    // NodeCache's lock.
    //
    // It has to happen here, not in CpuNodeMap::publish_all(): that hook runs at
    // InitLevel::CpuLocal + 1, while the slab allocator starts asking which
    // node it is on inside init_pmm(), several init levels earlier. And it
    // has to come before the nodes' init(), which reads
    // numa::Topology::firmware_node() to build each node's hard affinity.
    //
    // The possible mask is usually empty here: the MADT parse that fills it
    // (assign_cpunum_for_apic(), from platform_init_apic()) runs at
    // InitLevel::Irq, long after platform_init_pmm(). The exception is a NUMA
    // build on a machine with an SRAT, whose parse sets a possible bit for
    // every CPU it binds, at PrePmm + 1. Otherwise it is the explicit claim of
    // the running CPU that does the work -- and that would be needed
    // regardless: a machine that handed us no MADT still has the CPU this code
    // is executing on, and leaving that one on the sentinel is the exact
    // failure above. CpuNodeMap::claim_possible() claims the rest,
    // once the MADT has been parsed.
    //
    // The count it returns is said, because it is the firmware's placement
    // overruled: these CPUs are on a proximity domain with no memory, which
    // this kernel does not bring up as a node, so they run from the fallback
    // and belong to no node's hard affinity. Said here and not at the second
    // seed, which would only repeat the same CPUs.
    if (auto const refused = CpuNodeMap::seed_from_firmware(fallback, nodes); refused != 0) {
        log::info("pmm: the firmware put {} CPU(s) on a node with no memory; they run from node {}",
                  refused, fallback);
    }

    // Said here because here it is a fact, and said on every boot because the
    // interesting case is the one where it is 1 unexpectedly.
    //
    // x86_init_numa() cannot report this: it runs before the PMM, and
    // acpi_init_numa() applies the SRAT's memory affinities region by region
    // with nothing to roll them back, so a walk that stops part-way returns a
    // bad status with some nodes already registered. A node count announced
    // from there would be wrong in exactly that case. It was also wrong on an
    // SRAT-without-SLIT machine until a missing SLIT stopped failing the
    // discovery.
    //
    // This is also the only line on the subject a release image keeps: the
    // fallback-order dump that would otherwise contradict it is log::debug.
    log::info("pmm: {} node(s) own memory", nodes.count());

    // The zones before the nodes: cutting a node's memory into zones needs
    // the node's number and its range, both known from the boot memory map,
    // and no PmNode.
    PmZone::create_all(max_zone_pfn, nodes);

    if (auto const status = create_nodes(nodes); Status::Ok != status) {
        return status;
    }

    nodes.for_each([](NodeId const nid) {
        auto const status = find_node(nid)->init();
        ASSERT(status == Status::Ok);
    });

    connect_all(nodes);
    dump_fallback_queues(nodes);
    return Status::Ok;
}

auto PmNode::setup_watermarks() -> void {
    // Every zone's managed total, summed, is the denominator each zone's share
    // is taken out of.
    usize managed_total = 0;
    PmZone::for_each([&managed_total](PmZone &zone) { managed_total += zone.managed_frames(); });

    if (managed_total == 0) {
        // The division in setup_watermarks() would be by zero. bootstrap()
        // already refuses a machine whose nodes own no usable memory, so this
        // is the second belt rather than the first.
        return;
    }

    auto const reserve_frames = machine_reserve_frames(managed_total);
    PmZone::for_each([reserve_frames, managed_total](PmZone &zone) {
        zone.setup_watermarks(reserve_frames, managed_total, ZoneLayerKey{});
    });
    // After every zone's watermarks, though it reads only managed counts:
    // those are final once the reservations are counted, which is what this
    // function already waits for.
    PmZone::for_each([](PmZone &zone) { zone.setup_lowmem_reserve(ZoneLayerKey{}); });
}

/// Creates and registers a PmNode for every node in |nodes|.
///
/// A step of its own, ahead of init(), because the steps after it reach across
/// nodes: connect() walks every node's ZoneList, so the whole node table
/// has to be populated before any of it runs. Registration lives here rather
/// than at the end of init() for the same reason -- a node published only once
/// it has finished initialising is invisible to the node that initialises
/// before it, which is exactly the order bootstrap() runs them in.
///
/// EarlyMem is the only allocator up at this point, and asking it for node
/// `nid`'s memory keeps the structure on the node it describes. Nothing ever
/// frees these: a PmNode lives as long as the kernel does.
auto PmNode::create_nodes(NodeMask const &nodes) -> Status {
    auto status = Status::Ok;
    nodes.for_each([&status](NodeId const nid) {
        if (Status::Ok != status) {
            return;
        }
        auto *const node = EarlyMem::allocate<PmNode>(1, nid, bootmem::MemoryType::KernelData);
        if (!node) {
            status = Status::OutOfMem;
            return;
        }

        new (node) PmNode(nid);
        priv::register_node(nid, *node);
    });
    return status;
}

auto PmNode::init() -> Status {
    m_canary.verify<Self>();

    auto const [node_start_pfn, node_end_pfn] = EarlyMem::get_pfn_range_node(m_id);
    m_start_pfn = node_start_pfn;
    m_spanned_frames = node_end_pfn - node_start_pfn;

    // Online first: a node owns memory only once it is up (node_table.hpp).
    set_node_state(m_id, NodeState::Online);
    if (m_spanned_frames != 0) {
        set_node_state(m_id, NodeState::Memory);
    }

    for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
        join_hard_affinity(cpu, CpuPlacementKey{});
    }

    // The node's zones were cut before it was made (PmZone::create_all()): it
    // takes its row of the table, first in its own fallback order.
    m_zone_list.take_local_zones();

    // The table is not touched here: create_nodes() registers a node when it
    // creates it, before this runs. See the comment there.
    DEBUG_ASSERT(find_node(m_id) == this);
    return Status::Ok;
}

} // namespace ours::mm
