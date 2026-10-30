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
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>

#include <algorithm>

#include <ustl/bit.hpp>

namespace ours::mm {
namespace {
    /// Where each zone type starts and ends, in PFNs, for the whole machine.
    /// Set once by set_zone_bounds() and read by every PmNode::init() to cut
    /// its own span into zones; nothing needs them after boot.
    INIT_DATA
    Pfn ZONE_LOWER_PFN[NR_ZONES_PER_NODE];

    INIT_DATA
    Pfn ZONE_UPPER_PFN[NR_ZONES_PER_NODE];

    FORCE_INLINE
    auto clamp_pfn_to_zone(Pfn pfn, ZoneType ztype) -> Pfn {
        return std::clamp(pfn, ZONE_LOWER_PFN[static_cast<usize>(ztype)],
                          ZONE_UPPER_PFN[static_cast<usize>(ztype)]);
    }

    /// Marks Possible every node that owns a usable memory range.
    auto find_possible_nodes() -> Status {
        return EarlyMem::visit_usable(EarlyMem::min_address(), EarlyMem::max_address(),
                                      bootmem::ANY_NODE, [](bootmem::MemoryRegion const region) {
                                          PmNode::set_state(region.node, PmNode::State::Possible);
                                          return true;
                                      });
    }

    /// Lays the zone types end to end from the lowest usable PFN, each ending
    /// at its |max_zone_pfn| entry, the first PFN past what the caller allows
    /// that zone type. A zone type the machine has no memory for comes out
    /// empty, and so does one whose entry is 0, which is how the platform says
    /// it has none.
    ///
    /// One entry per type, by type: the loop reads every one. It once read
    /// five out of x86's list of three, past its end.
    auto set_zone_bounds(ustl::views::Span<Pfn> const max_zone_pfn) -> void {
        ASSERT(max_zone_pfn.size() == std::size(ZONE_LOWER_PFN),
               "{} zone bounds for {} zone types: one is needed for each, by type",
               max_zone_pfn.size(), std::size(ZONE_LOWER_PFN));
        auto start_pfn = phys_to_pfn(EarlyMem::min_address());
        for (usize i = 0; i < std::size(ZONE_LOWER_PFN); i++) {
            auto const end_pfn = std::max(max_zone_pfn[i], start_pfn);
            ZONE_LOWER_PFN[i] = start_pfn;
            ZONE_UPPER_PFN[i] = end_pfn;
            start_pfn = end_pfn;
        }
    }

    /// The first node in |nodes|, or NUMA_MAX_NODE when there is none.
    auto first_node(NodeMask const &nodes) -> NnId {
        auto first = NnId(NUMA_MAX_NODE);
        nodes.for_each([&first](NnId const nid) {
            if (first == NUMA_MAX_NODE) {
                first = nid;
            }
        });
        return first;
    }

    /// Gives every CPU that no topology source claimed the node |fallback|.
    ///
    /// Topology's two CPU-to-node tables start out filled with NUMA_MAX_NODE
    /// -- the "no node known yet" sentinel -- and the only thing that ever
    /// overwrites it is a topology source: on x86 the SRAT parse, which a
    /// kernel built without NUMA never compiles in and a machine without an
    /// SRAT never feeds. Left on the sentinel, that is what current_node()
    /// returns, and its callers index NUMA_MAX_NODE-long arrays with it --
    /// MemCache::allocate() reads m_node_caches[NUMA_MAX_NODE], one past the
    /// end, takes the ListMemberHook that follows the array for a NodeCache,
    /// and spins forever on a list pointer it has mistaken for that
    /// NodeCache's lock.
    ///
    /// It has to happen here, not in PmNode::Current::init(): that hook runs at
    /// InitLevel::CpuLocal + 1, while the slab allocator starts asking which
    /// node it is on inside init_pmm(), several init levels earlier. And it
    /// has to come before the nodes' init(), which reads
    /// Topology::firmware_node() to build each node's hard affinity.
    ///
    /// The possible mask is empty here: the MADT parse that fills it
    /// (assign_cpunum_for_apic(), from platform_init_apic()) runs at
    /// InitLevel::Irq, long after platform_init_pmm(). The walk is kept for the
    /// day the parse moves ahead of the PMM, but today it is the explicit claim
    /// of the running CPU that does the work -- and it would be needed
    /// regardless: a machine that handed us no MADT still has the CPU this code
    /// is executing on, and leaving that one on the sentinel is the exact
    /// failure above. PmNode::Current::claim_possible_cpus() claims the rest,
    /// once the MADT has been parsed.
    auto claim_unplaced_cpus(NnId const fallback) -> void {
        auto const claim_cpu = [fallback](CpuNum const cpu) {
            if (PmNode::Topology::cpu_to_node(cpu) == NUMA_MAX_NODE) {
                PmNode::Topology::set_cpu_node_early(cpu, fallback);
            }
        };
        Cpu::possible_mask().for_each(claim_cpu);
        claim_cpu(CpuLocal::current_cpu());
    }

    /// Chains every node's zones into its fallback order. Needs every node to
    /// exist and be initialised first: connect() walks the other nodes'
    /// ZoneList.
    auto connect_nodes(NodeMask const &nodes) -> void {
        nodes.for_each(
            [&nodes](NnId const nid) { PmNode::node(nid)->zone_list()->connect(nodes); });
    }

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
        nodes.for_each([](NnId const nid) { PmNode::node(nid)->zone_list()->dump_fallback(); });
    }
} // namespace

/// Brings up every node, in the order each step depends on: which node is
/// which and which CPUs are on it, then the nodes and their zones, then the
/// fallback order that chains them.
auto PmNode::bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status {
    ASSERT(!max_zone_pfn.empty());

    if (auto const status = find_possible_nodes(); Status::Ok != status) {
        return status;
    }
    set_zone_bounds(max_zone_pfn);

    auto const &nodes = node_possible_mask();
    auto const fallback = first_node(nodes);
    if (fallback == NUMA_MAX_NODE) {
        // No node owns usable memory; the steps below would build nothing and
        // every allocation would fail somewhere less obvious than here.
        return Status::BadState;
    }
    claim_unplaced_cpus(fallback);

    if (auto const status = create_nodes(nodes); Status::Ok != status) {
        return status;
    }

    nodes.for_each([](NnId const nid) {
        auto const status = PmNode::node(nid)->init();
        ASSERT(status == Status::Ok);
    });

    connect_nodes(nodes);
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
        zone.setup_watermarks(reserve_frames, managed_total);
    });
}

/// Creates and registers a PmNode for every node in |nodes|.
///
/// A step of its own, ahead of init(), because the steps after it reach across
/// nodes: connect() walks every node's ZoneList, so the whole NODES table
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
    nodes.for_each([&status](NnId const nid) {
        if (Status::Ok != status) {
            return;
        }
        auto *const node = EarlyMem::allocate<PmNode>(1, nid, bootmem::MemoryType::KernelData);
        if (!node) {
            status = Status::OutOfMem;
            return;
        }

        new (node) PmNode(nid);
        NODES[nid] = node;
        NUM_NODES += 1;
    });
    return status;
}

auto PmNode::init() -> Status {
    m_canary.verify<Self>();

    auto const [node_start_pfn, node_end_pfn] = EarlyMem::get_pfn_range_node(m_id);
    m_start_pfn = node_start_pfn;
    m_spanned_frames = node_end_pfn - node_start_pfn;

    if (m_spanned_frames != 0) {
        set_state(m_id, State::Memory);
    }
    set_state(m_id, State::Online);

    for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
        if (Topology::firmware_node(cpu) == m_id) {
            m_hard_affinity.set(cpu);
        }
    }

    usize total_present_frames = 0;
    for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
        auto const ztype = ZoneType(i);
        auto const zone_start_pfn = clamp_pfn_to_zone(node_start_pfn, ztype);
        auto const zone_end_pfn = clamp_pfn_to_zone(node_end_pfn, ztype);

        if (zone_start_pfn < node_start_pfn || zone_end_pfn > node_end_pfn ||
            zone_end_pfn <= zone_start_pfn) {
            continue;
        }

        auto zone = EarlyMem::allocate<PmZone>(1, m_id, bootmem::MemoryType::KernelData);
        if (!zone) {
            panic("No enough memory to place node[{}].zone[{}]", m_id, to_string(ztype));
        }

        auto const num_present_frames =
            EarlyMem::count_present_frames(zone_start_pfn, zone_end_pfn);
        ASSERT(num_present_frames > 1);

        new (zone) PmZone(m_id, ztype, zone_start_pfn, zone_end_pfn, num_present_frames);

        zone_list()->insert_local_zone(zone, ztype);

        total_present_frames += num_present_frames;
        PmNode::set_state(m_id, State(ztype));
    }

    m_present_frames = total_present_frames;

    // NODES[m_id] and NUM_NODES are not touched here: bootstrap() publishes a
    // node when it creates it, before this runs. See the comment there.
    DEBUG_ASSERT(NODES[m_id] == this);
    return Status::Ok;
}

} // namespace ours::mm
