#include <ours/assert.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>

#include <algorithm>

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
/// contributes its own zones from its highest type down, as this node's own
/// come first (push_zones_of()).
///
/// It used to take a NodeMask and call Router::build() to turn it into an
/// order, which meant this function read the machine's distance matrix. The
/// derivation is numa::build_fallback_orders(), asked once by bring-up; what
/// arrives here is already a list of node ids, and the only thing left to
/// decide is nothing.
auto ZoneList::connect(ustl::views::Span<NodeId const> const order) -> void {
    for (auto const next: order) {
        DEBUG_ASSERT(next != m_nid, "node {} was given itself as a fallback hop", m_nid);
        DEBUG_ASSERT(find_node(next), "node {} is a fallback hop but was not brought up", next);
        push_zones_of(next);
    }
}

auto ZoneList::dump_fallback() const -> void {
    log::debug("Node[{}-fallback]", m_nid);
    for (auto const *const zone: m_fallback) {
        log::debug("    None-{}| ZoneType-{:<6}| [0x{:X}, 0x{:X})", zone->which_node(),
                   to_string(zone->zone_type()), zone->start_pfn(), zone->end_pfn());
    }
}

PmNode::PmNode(NodeId nid)
    : m_canary(this)
    , m_id(nid)
    , m_lock()
    , m_hard_affinity()
    , m_soft_affinity()
    , m_start_pfn()
    , m_zone_list(nid)
    , m_page_queues()
    , m_short_zones(0) {}

auto PmNode::alloc_frames(AllocRequest const &request, AdmissionPass const pass)
    -> ustl::Result<PmFrame *, BlockFailure> {
    m_canary.verify<Self>();
    DEBUG_ASSERT(request.home_node() == m_id, "node {} was handed a request whose home is node {}",
                 m_id, request.home_node());

    ustl::Option<PmZone const &> depleted_zone;
    for (auto &zone: m_zone_list.candidates(request)) {
        // Taking the block would leave the zone below the pass's floor: short
        // for this request. Passed over, and the first such zone named in the
        // failure, should the walk fail.
        if (!zone.admits(request.order(), pass, request.may_block(), request.highest_zone())) {
            if (!depleted_zone) {
                depleted_zone = ustl::some_ref(zone);
            }
            continue;
        }

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

auto PmNode::free_frames(PmFrame &frame, usize const order) -> bool {
    m_canary.verify<Self>();
    DEBUG_ASSERT(frame.nid() == m_id, "frame {:#x} of node {} was returned to node {}",
                 frame_to_phys(&frame), frame.nid(), m_id);

    auto *const zone = m_zone_list.local_zone(frame.zone());
    DEBUG_ASSERT(zone, "frame {:#x} is in no zone", frame_to_phys(&frame));

    return zone->free_frames(frame, order);
}

namespace {
    /// |type|'s bit in a node's record of short zones.
    constexpr auto short_zone_bit(ZoneType const type) -> u32 {
        static_assert(NR_ZONES_PER_NODE <= 32, "the record holds one bit per zone type in a u32");
        return u32{1} << std::to_underlying(type);
    }
} // namespace

auto PmNode::mark_short(PmZone const &zone) -> bool {
    m_canary.verify<Self>();
    DEBUG_ASSERT(zone.which_node() == m_id, "a zone of node {} was marked short on node {}",
                 zone.which_node(), m_id);

    // The bits carry no other data with them -- the take reads the zones'
    // counts afresh -- so the order only has to keep each read-modify-write
    // whole, which any order does. acq_rel is the order the record had in
    // the reclaimer, kept.
    return m_short_zones.fetch_or(short_zone_bit(zone.zone_type()), std::memory_order_acq_rel) == 0;
}

auto PmNode::take_reclaim_target() -> usize {
    m_canary.verify<Self>();

    auto const marked = m_short_zones.exchange(0, std::memory_order_acq_rel);
    if (marked == 0) {
        return 0;
    }

    // Each marked zone is read again: the mark says an allocation found it
    // short, not how short it is now.
    usize wanted = 0;
    m_zone_list.for_each_local_zone([marked, &wanted](PmZone const &zone) {
        if ((marked & short_zone_bit(zone.zone_type())) == 0) {
            return;
        }
        auto const free = zone.num_free_frames();
        auto const high = zone.watermark(PmZone::Watermark::High);
        if (free < high) {
            wanted += high - free;
        }
    });
    return wanted;
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

auto PmNode::add_cpu(CpuNum const cpu) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(cpu < MAX_CPU);

    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    m_work_cpus.set(cpu);
}

auto PmNode::remove_cpu(CpuNum const cpu) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(cpu < MAX_CPU);

    Guard<sync::SpinLock, IrqSave> guard{get_lock()};
    m_work_cpus.set(cpu, 0);
}

auto PmNode::join_hard_affinity(CpuNum const cpu) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(cpu < MAX_CPU);

    if (numa::Topology::firmware_node(cpu) == m_id) {
        m_hard_affinity.set(cpu);
    }
}

} // namespace ours::mm
