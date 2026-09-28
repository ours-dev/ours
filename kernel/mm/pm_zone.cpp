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
    /// The zone lock is taken with interrupts off, as Linux takes zone->lock
    /// (spin_lock_irqsave in rmqueue_bulk() and free_one_page()).
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
    , m_spanned_frames(end_pfn - start_pfn)
    , m_frame_cache() {}

PmZone::~PmZone() {}

auto PmZone::finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void {
    m_managed_frames -= BIT(order);
}

auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset_attached) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
        m_fset_attached = true;
    }
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
    ensure_fset_attached();

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
        std::fill_n(frame_to_virt<isize>(frame), BIT(order) * PAGE_SIZE / sizeof(usize), 0);
    }

    finish_allocation(frame, gaf, order);
    return frame;
}

auto PmZone::free_frames() -> usize {
    ZoneGuard guard{m_lock};
    return m_fset.free_frames();
}

auto PmZone::contains(usize order) -> bool {
    ensure_fset_attached();

    ZoneGuard guard{m_lock};
    return m_fset.has_free_block(order);
}

auto PmZone::free_frame(PmFrame *frame, usize order) -> void {
    ensure_fset_attached();

    if (is_order_within_pcpu_cache_limit(order)) {
        m_frame_cache.with_current([](PcpuCache &cache) {
            // Here no pcpu cache.
        });
    } else {
        ZoneGuard guard{m_lock};
        m_fset.release_frame(frame, order);
    }
    m_managed_frames += BIT(order);
}
} // namespace ours::mm