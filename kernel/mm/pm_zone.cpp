#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>

#include <algorithm>

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

PmZone::PmZone(NnId nid, ZoneType type, Pfn start_pfn, Pfn end_pfn, usize present_frames)
    // Initializer order follows the member declaration order in pm-zone.hpp.
    : m_canary(this)
    , m_name(to_string(type))
    // m_node is filled in by PmNode once it adopts this zone; a zone is built
    // before the node's queues are wired, so there is nothing to point at yet.
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
    , m_frame_cache()
    // Set up here, once, for every zone -- one the handoff frees nothing into
    // included. No CPU can see the zone yet: PmNode::init() puts it on the
    // node's zone list only after this returns. So it needs no lock, and the
    // paths that use the set never have to ask whether it is ready -- which,
    // asked outside the zone lock on every allocation, was a race between two
    // CPUs meeting a fresh zone at once.
    //
    // The span is all it takes. It used to take the descriptor of the zone's
    // first pfn too, and find every other descriptor by offset from that one,
    // which reads the zone's descriptors as one array; the sparse memmap
    // allocates them a section at a time, apart.
    , m_fset(start_pfn, end_pfn) {
    [[maybe_unused]]
    auto const enrolled = ZONES.push_back(this);
    ASSERT(enrolled.is_ok(), "more zones than {} nodes of {} hold", NUMA_MAX_NODE,
           NR_ZONES_PER_NODE);
}

PmZone::~PmZone() {}

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

    m_watermarks[static_cast<usize>(WaterMark::Min)] = min;
    m_watermarks[static_cast<usize>(WaterMark::Low)] = min + delta;
    m_watermarks[static_cast<usize>(WaterMark::High)] = min + delta + delta;

    // One line, and a short one on purpose. logz4 hands std::vformat_to a
    // bounded output iterator, and libc++ gives up on its own 256-byte buffer
    // for a body longer than that and retargets to an allocating one -- which
    // reaches the plain operator new the kernel defines as a panic. A log line
    // is therefore a fixed-size resource, and the limit is silent until it
    // panics naming operator new.
    log::info("Zone[{}]: managed {} frames, watermarks {}/{}/{}", m_name, managed, min, min + delta,
              min + delta + delta);
}

auto PmZone::finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void {
    m_free_frames -= usize{1} << order;
}

auto PmZone::alloc_frames(Gaf gaf, usize order, AllocationOptions options,
                          MemoryAllocationContext const &context) -> PmFrame * {
    // A zone below its high watermark is running short. A caller that is able
    // to wait should move on to the next zone and let reclaim catch up here;
    // one that cannot block has nowhere else to go, so it is allowed to dip.
    if (is_below_high_wmark() && !(options & AllocationOptions::NoBlock)) {
        return nullptr;
    }

    return alloc_frame(gaf, order);
}

auto PmZone::alloc_frame(Gaf gaf, usize order) -> PmFrame * {
    PmFrame *frame = nullptr;
    if (is_order_within_pcpu_cache_limit(order)) {
        frame = m_frame_cache.with_current([](PcpuCache &cache) { return nullptr; });
    }

    if (!frame) {
        // The zone's lock covers the frame set; the set has none of its own.
        // Held only across the buddy call, so the zeroing below -- which can be
        // a whole max-order block -- happens outside it. IrqSave: see
        // ZoneGuard.
        ZoneGuard guard{m_lock};
        frame = m_fset.acquire_frame(order);
    }

    if (!frame) {
        return nullptr;
    }

    if (!!(Gaf::Zero & gaf)) {
        std::fill_n(frame_to_virt<isize>(frame), (usize{1} << order) * PAGE_SIZE / sizeof(usize),
                    0);
    }

    finish_allocation(frame, gaf, order);
    return frame;
}

auto PmZone::count_free_frames() -> usize {
    ZoneGuard guard{m_lock};
    return m_fset.free_frames();
}

auto PmZone::free_blocks(usize order) -> usize {
    ZoneGuard guard{m_lock};
    return m_fset.free_blocks(order);
}

auto PmZone::contains(usize order) -> bool {
    ZoneGuard guard{m_lock};
    return m_fset.has_free_block(order);
}

auto PmZone::free_frame(PmFrame *frame, usize order) -> void {
    if (is_order_within_pcpu_cache_limit(order)) {
        m_frame_cache.with_current([](PcpuCache &cache) {
            // Here no pcpu cache.
        });
    } else {
        // The buddy finds the partners from the freed descriptor and its pfn,
        // never from a zone-wide base. Read before the lock: the memmap does
        // not change under a frame the caller holds.
        auto const pfn = frame_to_pfn(frame);
        ZoneGuard guard{m_lock};
        m_fset.release_frame(frame, pfn, order);
    }
    m_free_frames += usize{1} << order;
}
} // namespace ours::mm