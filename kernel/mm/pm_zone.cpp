#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <logz4/log.hpp>
namespace ours::mm {
namespace {
    /// The zone lock is taken with interrupts off, on the allocation and the
    /// free path alike.
    ///
    /// It used to be `Guard guard{m_lock}`, and a Guard with no option saves no
    /// state: it neither disables interrupts nor holds off preemption
    /// (lockdep::Guard builds one saver per option, and there were none). So a
    /// thread could be preempted holding a zone lock, and the next thread on
    /// that CPU to free a frame in the zone found the lock "held by this CPU":
    /// the debug build asserted (SpinLock::acquire(), !is_held()), a release
    /// build would have spun until it was itself preempted. It surfaced once
    /// dead processes started giving their memory back from a DPC thread,
    /// which frees in bursts.
    ///
    /// IrqSave rather than PreemptDisable: the PMM is used before there are
    /// threads to hold preemption off for, and a frame freed from an interrupt
    /// must not meet the lock held underneath it either.
    using ZoneGuard = Guard<sync::SpinLock, IrqSave>;

    /// The gap between two of a zone's watermarks, as a fraction of the zone:
    /// WATERMARK_SCALE out of WATERMARK_SCALE_UNIT, so 0.1% by default.
    ///
    /// It is what decides how much room a reclaimer has to work in -- it is
    /// woken when the zone drops below High and has this much to go before
    /// allocations start being turned away at Low.
    constexpr auto WATERMARK_SCALE = usize(10);
    constexpr auto WATERMARK_SCALE_UNIT = usize(10000);
} // namespace

PmZone::PmZone(NodeId nid, ZoneType type, Pfn start_pfn, Pfn end_pfn, usize present_frames)
    // Initializer order follows the member declaration order in pm-zone.hpp.
    : m_canary(this)
    , m_name(to_string(type))
    // m_node is filled in by PmNode::init(), when the node takes its row of the
    // table: the zone is made before any node exists.
    , m_node(nullptr)
    , m_nid(nid)
    , m_flags(Flags::Initialized)
    , m_type(type)
    , m_start_pfn(start_pfn)
    , m_present_frames(present_frames)
    // Zero because the buddy owns nothing yet: the handoff releases this
    // zone's frames into it afterwards, each release counting itself in.
    , m_free_frames(0)
    , m_spanned_frames(end_pfn - start_pfn)
    // Both were left out of this list although the comment above says it
    // follows the declaration order. std::atomic and std::array value-
    // initialise themselves, so the values were right by accident; being
    // explicit is what the rest of the list does.
    , m_reserved_frames(0)
    , m_watermarks()
    , m_lowmem_reserve()
    , m_frame_cache()
    // Set up here, once, for every zone -- one the handoff frees nothing into
    // included. No CPU can see the zone yet: create_all() puts it in the
    // table only after this returns. So it needs no lock, and the
    // paths that use the set never have to ask whether it is ready -- which,
    // asked outside the zone lock on every allocation, was a race between two
    // CPUs meeting a fresh zone at once.
    //
    // The span is all it takes. It used to take the descriptor of the zone's
    // first pfn too, and find every other descriptor by offset from that one,
    // which reads the zone's descriptors as one array; the sparse memmap
    // allocates them a section at a time, apart.
    , m_buddy(start_pfn, end_pfn) {}

PmZone::~PmZone() {}

INIT_CODE
auto PmZone::create_all(ustl::views::Span<Pfn> const max_zone_pfn, NodeMask const &nodes) -> void {
    ASSERT(max_zone_pfn.size() == NR_ZONES_PER_NODE,
           "{} zone bounds for {} zone types: one is needed for each, by type", max_zone_pfn.size(),
           NR_ZONES_PER_NODE);

    // The types end to end from the lowest usable PFN, each up to its bound. A
    // type whose bound is 0, or below where the types before it end, comes out
    // empty there.
    std::array<Pfn, NR_ZONES_PER_NODE> lower{};
    std::array<Pfn, NR_ZONES_PER_NODE> upper{};
    auto start_pfn = phys_to_pfn(EarlyMem::min_address());
    for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
        auto const end_pfn = std::max(max_zone_pfn[i], start_pfn);
        lower[i] = start_pfn;
        upper[i] = end_pfn;
        start_pfn = end_pfn;
    }

    nodes.for_each([&lower, &upper](NodeId const nid) {
        auto const [node_start_pfn, node_end_pfn] = EarlyMem::get_pfn_range_node(nid);
        for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
            auto const ztype = ZoneType(i);
            auto const zone_start_pfn = std::clamp(node_start_pfn, lower[i], upper[i]);
            auto const zone_end_pfn = std::clamp(node_end_pfn, lower[i], upper[i]);
            if (zone_end_pfn <= zone_start_pfn) {
                continue;
            }

            // From the node's own memory: the boot allocator searches only the
            // node it is given.
            auto *const zone = EarlyMem::allocate<PmZone>(1, nid, bootmem::MemoryType::KernelData);
            if (!zone) {
                panic("No enough memory to place node[{}].zone[{}]", nid, to_string(ztype));
            }

            auto const num_present_frames =
                EarlyMem::count_present_frames(zone_start_pfn, zone_end_pfn);
            ASSERT(num_present_frames > 1);

            new (zone) PmZone(nid, ztype, zone_start_pfn, zone_end_pfn, num_present_frames);
            TABLE[nid][i] = zone;
        }
    });
}

auto PmZone::setup_watermarks(usize const reserve_frames, usize const managed_total) -> void {
    DEBUG_ASSERT(managed_total != 0, "no zone manages a single frame");

    auto const managed = managed_frames();

    // This zone's share of the reserve, in proportion to the memory it
    // manages: a zone holding a tenth of the machine's frames holds back a
    // tenth of the reserve. Proportional rather than equal, so that the floor
    // costs every zone the same fraction of itself instead of costing a small
    // zone most of what it has -- a 16 MiB Dma zone and a 500 MiB Normal zone
    // cannot afford the same number of frames.
    auto const min = reserve_frames * managed / managed_total;

    // A quarter of this zone's floor, or WATERMARK_SCALE of the zone, whichever
    // is larger. The floor grows with the square root of memory, so on a large
    // machine a quarter of it is too narrow a band to reclaim in: by the time
    // a reclaimer woken at High had run, allocations would already be hitting
    // Low. The fraction of the zone is what keeps the band usable there.
    auto const delta = std::max(min >> 2, managed * WATERMARK_SCALE / WATERMARK_SCALE_UNIT);

    m_watermarks[std::to_underlying(Watermark::Min)] = min;
    m_watermarks[std::to_underlying(Watermark::Low)] = min + delta;
    m_watermarks[std::to_underlying(Watermark::High)] = min + delta + delta;

    // One line, and a short one on purpose. logz4 hands std::vformat_to a
    // bounded output iterator, and libc++ gives up on its own 256-byte buffer
    // for a body longer than that and retargets to an allocating one -- which
    // reaches the plain operator new the kernel defines as a panic. A log line
    // is therefore a fixed-size resource, and the limit is silent until it
    // panics naming operator new.
    log::info("Zone[{}]: managed {} frames, watermarks {}/{}/{}", m_name, managed, min, min + delta,
              min + delta + delta);
}

auto PmZone::setup_lowmem_reserve() -> void {
    // The node's managed frames by type: the input mm::lowmem_reserve() wants.
    std::array<usize, NR_ZONES_PER_NODE> managed{};
    auto const row = PmZone::row(m_nid);
    for (usize type = 0; type < NR_ZONES_PER_NODE; ++type) {
        if (auto const *const zone = row[type]) {
            managed[type] = zone->managed_frames();
        }
    }

    auto const own = std::to_underlying(m_type);
    for (usize highest = 0; highest < NR_ZONES_PER_NODE; ++highest) {
        m_lowmem_reserve[highest] =
            mm::lowmem_reserve(managed.data(), managed.size(), own, highest, LOWMEM_RESERVE_RATIO);
    }

    // One short line, for the reason setup_watermarks() gives: the reserve
    // against a request that allows every zone up to Normal, the one every
    // allocation makes today.
    log::info("Zone[{}]: keeps {} frames from requests that allow Normal", m_name,
              m_lowmem_reserve[std::to_underlying(ZoneType::Normal)]);
}

auto PmZone::alloc_frames(usize order) -> ustl::Option<PmFrame &> {
    // A zone below its high watermark is running short, and this gate still
    // has no writer for the flag, so it never fires. It also tests the wrong
    // line: the high watermark is where a zone becomes worth reclaiming, not
    // where allocations are refused. Both are the admission gate's own change,
    // which moves this decision out of the zone altogether -- see
    // .agents/tasks/pmm-layering/PLAN.md step 8. The term that distinguished a
    // caller able to wait went with AllocationOptions, which was always empty
    // and so never distinguished anything.
    if (is_below_high_wmark()) {
        return ustl::NONE;
    }

    if (is_order_within_pcpu_cache_limit(order)) {
        auto const cached = m_frame_cache.with_current(
            [](PcpuCache &cache) { return ustl::Option<PmFrame &>{ustl::NONE}; });
        if (cached) {
            m_free_frames -= usize{1} << order;
            return cached;
        }
    }

    ZoneGuard guard{m_lock};
    auto const taken = m_buddy.allocate(order);
    if (taken) {
        m_free_frames -= usize{1} << order;
    }
    return taken;
}

auto PmZone::count_free_frames() -> usize {
    // Every block on every order's list, weighted by its size: a walk, not a
    // counter, so it costs time proportional to how fragmented the zone is.
    // That is what makes this the diagnostic form and num_free_frames() the one
    // the allocation path asks.
    ZoneGuard guard{m_lock};
    usize total = 0;
    for (usize order = 0; order < FrameBuddy::NUM_ORDERS; ++order) {
        total += m_buddy.free_count(order) << order;
    }
    return total;
}

auto PmZone::free_blocks(usize order) -> usize {
    ZoneGuard guard{m_lock};
    return m_buddy.free_count(order);
}

auto PmZone::contains(usize order) -> bool {
    ZoneGuard guard{m_lock};
    // Some free block of at least |order|, which is at most one emptiness
    // test per order above it.
    for (auto o = order; o < FrameBuddy::NUM_ORDERS; ++o) {
        if (!m_buddy.empty(o)) {
            return true;
        }
    }
    return false;
}

auto PmZone::free_frames(PmFrame &frame, usize order) -> bool {
    auto const count = usize{1} << order;

    // The count this release's own increment found. An atomic read-modify-
    // write, so the increments and the allocations' decrements fall in one
    // order: every time the count climbs over a line, exactly one increment
    // is the one that took it over, and it is the only one that sees so.
    // Reading the counter before and after the release instead lets two
    // releases with an allocation between them both see no crossing.
    auto const before = [&] {
        if (is_order_within_pcpu_cache_limit(order)) {
            m_frame_cache.with_current([](PcpuCache &cache) {
                // Here no pcpu cache.
            });
            return m_free_frames.fetch_add(count);
        }

        auto const pfn = frame_to_pfn(&frame);
        ZoneGuard guard{m_lock};
        m_buddy.release(&frame, pfn, order);
        return m_free_frames.fetch_add(count);
    }();

    // The line is where the Low pass starts admitting one frame again for a
    // request that allows up to Normal (normal_low_floor()), with the same
    // arithmetic the pass itself uses. Low alone would be one frame early --
    // at exactly Low the pass still refuses, since a frame taken there would
    // leave Low - 1.
    auto const floor = normal_low_floor();
    return !mm::admits(before, 0, floor) && mm::admits(before + count, 0, floor);
}
} // namespace ours::mm
