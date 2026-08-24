#include <ours/mm/pm-zone.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/memory-model.hpp>

#include <ours/init.hpp>
#include <ours/assert.hpp>
#include <ours/mm/cpu-local.hpp>

#include <logz4/log.hpp>
#include <ustl/algorithms/generation.hpp>

namespace ours::mm {

    PmZone::PmZone(NnId nid, ZoneType type, Pfn start_pfn, Pfn end_pfn, usize present_frames)
        : m_canary(this)
        , m_name(to_string(type))
        , m_type(type)
        , m_start_pfn(start_pfn)
        , m_spanned_frames(end_pfn - start_pfn)
        , m_present_frames(present_frames)
        , m_flags(Flags::Initialized)
        , m_frame_cache()
    {}

    PmZone::~PmZone()
    {}

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

    auto PmZone::alloc_frames(Gaf gaf, usize order, AllocationControl control, MemoryAllocationContext const &context) 
        -> PmFrame * {
        return nullptr;
    }

    auto PmZone::alloc_frame(Gaf gaf, usize order) -> PmFrame * {
        ensure_fset_attached();

        PmFrame *frame = nullptr;
        if (is_order_within_pcpu_cache_limit(order)) {
            frame = m_frame_cache.with_current([order] (PcpuCache &cache) {
                return nullptr;
            });
        }

        if (!frame) {
            frame = m_fset.acquire_frame(order);
        }

        if (!frame) {
            return nullptr;
        }

        if (!!(Gaf::Zero & gaf)) {
            ustl::algorithms::fill_n(frame_to_virt<isize>(frame), BIT(order) * PAGE_SIZE / sizeof(usize), 0);
        }

        finish_allocation(frame, gaf, order);
        return frame;
    }

    auto PmZone::free_frame(PmFrame *frame, usize order) -> void {
        ensure_fset_attached();

        if (is_order_within_pcpu_cache_limit(order)) {
            m_frame_cache.with_current([frame] (PcpuCache &cache) {
                // Here no pcpu cache.
            });
        } else {
            m_fset.release_frame(frame, order);
        }
        m_managed_frames += BIT(order);
    }
}