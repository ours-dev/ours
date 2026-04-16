#include <ours/mem/pm-zone.hpp>
#include <ours/mem/pm-node.hpp>
#include <ours/mem/memory-model.hpp>

#include <ours/init.hpp>
#include <ours/assert.hpp>
#include <ours/mem/cpu-local.hpp>

#include <logz4/log.hpp>
#include <ustl/algorithms/generation.hpp>

namespace ours::mem {
namespace {
    FORCE_INLINE
    auto frame_is_buddy(PmFrame *self, PmFrame *buddy) -> bool {
        if (!buddy->is_role(PageFrame::Role::Pmm)) {
            return false;
        }
        if (self->zone() != buddy->zone()) {
            return false;
        }
        if (self->nid() != buddy->nid()) {
            return false;
        }
        if (self->order() != buddy->order()) {
            return false;
        }

        return true;
    }

    FORCE_INLINE
    auto get_buddy(PmFrame *frame, Pfn pfn, usize order) -> PmFrame * {
        auto const buddy_pfn = pfn + (1 << order);
        if (auto buddy_frame = pfn_to_frame(buddy_pfn)) {
            if (frame_is_buddy(frame, buddy_frame)) {
                return buddy_frame;
            }
        }

        return nullptr;
    }

    FORCE_INLINE
    auto get_buddy_unchecked(PmFrame *frame, Pfn pfn, usize order) -> PmFrame * {
        return &frame[BIT(order)];
    }
} // namespace anonymous

    FORCE_INLINE
    auto FrameSet::has_frame(usize order) const -> bool {
        return !lists_[order].empty();
    }

    FORCE_INLINE
    auto FrameSet::get_frame(usize order) -> PmFrame * {
        return &lists_[order].front();
    }

    FORCE_INLINE
    auto FrameSet::remove_frame(PmFrame *frame, usize order) -> void {
        auto const to_erase = lists_[order].iterator_to(*frame);
        lists_[order].erase(to_erase);
        frame->set_role(PageFrame::Role::None);
    }

    FORCE_INLINE
    auto FrameSet::insert_frame(PmFrame *frame, usize order) -> void {
        lists_[order].push_back(*frame);
        frame->set_role(PageFrame::Role::Pmm);
        frame->set_order(order);
    }

    auto FrameSet::acquire_frame_inner(PmFrame *frame, usize lower_order, usize upper_order) -> PmFrame * {
        auto const pfn = frame_to_pfn(frame);
        while (upper_order > lower_order) {
            upper_order -= 1;

            // In split process, directly taking a buddy frame is absolutely safe.
            auto buddy = get_buddy_unchecked(frame, pfn, upper_order);
            insert_frame(buddy, upper_order);
        }
        frame->set_order(lower_order);

        return frame;
    }

    auto FrameSet::acquire_frame_locked(usize target_order) -> PmFrame * {
        for (auto order = target_order; order < kNumOrders; ++order) {
            if (!has_frame(order)) {
                continue;
            }
            
            auto frame = get_frame(order);
            remove_frame(frame, order);
            acquire_frame_inner(frame, target_order, order);
            return frame;
        }

        return nullptr;
    }

    auto FrameSet::release_frame_inner(PmFrame *frame, Pfn pfn, usize order) -> void {
        // Do fold
        for (auto i = order; i < kMaxFrameOrder; ++i) {
            auto buddy_frame = get_buddy(frame, pfn, order);
            if (!buddy_frame) {
                break;
            }

            remove_frame(buddy_frame, order);
            order += 1;
        }

        insert_frame(frame, order);
    }

    auto FrameSet::release_frame_locked(PmFrame *frame, usize order) -> void {
        auto pfn = frame_to_pfn(frame);
        auto const end_pfn = pfn + (1 << order);

        order = ustl::algorithms::min(order, kMaxFrameOrder);
        while (pfn != end_pfn) {
            release_frame_inner(frame, pfn, order);
            pfn += 1 << order;
            frame = pfn_to_frame(pfn);
        }
    }

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

    auto PmZone::alloc_frames(Gaf gaf, usize order, AllocationControl control, MemoryAllocationContext const &context) 
        -> PmFrame * {
        return nullptr;
    }

    auto PmZone::alloc_frame(Gaf gaf, usize order) -> PmFrame * {
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