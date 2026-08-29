#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/memory_model.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>

#include <algorithm>
namespace ours::mm {

PmZone::PmZone(NnId nid, ZoneType type, Pfn start_pfn, Pfn end_pfn, usize present_frames)
    // Initializer order follows the member declaration order in pm-zone.hpp.
    : m_canary(this)
    , m_name(to_string(type))
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
        m_fset.attach(MemoryModel::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
        m_fset_attached = true;
    }
}

auto PmZone::alloc_frames(Gaf gaf, usize order, AllocationControl control,
                          MemoryAllocationContext const &context) -> PmFrame * {
    // A zone below its high watermark is running short. A caller that is able
    // to wait should move on to the next zone and let reclaim catch up here;
    // one that cannot block has nowhere else to go, so it is allowed to dip.
    if (is_below_high_wmark() && !(control & AllocationControl::NoBlock)) {
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

auto PmZone::free_frame(PmFrame *frame, usize order) -> void {
    ensure_fset_attached();

    if (is_order_within_pcpu_cache_limit(order)) {
        m_frame_cache.with_current([](PcpuCache &cache) {
            // Here no pcpu cache.
        });
    } else {
        m_fset.release_frame(frame, order);
    }
    m_managed_frames += BIT(order);
}
} // namespace ours::mm