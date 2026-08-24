/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/core/frame-buddy.hpp>

#include <ustl/algorithms/minmax.hpp>

namespace ours::mm::core {
    FrameBuddy::FrameBuddy(PmFrame *frames, usize count, Pfn base_pfn)
        : m_frames(frames)
        , m_count(count)
        , m_base_pfn(base_pfn)
        , m_lists() {}

    auto FrameBuddy::attach(PmFrame *frames, usize count, Pfn base_pfn) -> void {
        m_frames = frames;
        m_count = count;
        m_base_pfn = base_pfn;
    }

    auto FrameBuddy::acquire_inner(PmFrame *frame, Order lower_order, Order upper_order) -> PmFrame * {
        // Split downward: at each step the freshly split upper half becomes the
        // buddy at that order. frame is always the lower half here, so pointer
        // arithmetic (&frame[BIT(order)]) is exactly the buddy.
        while (upper_order > lower_order) {
            upper_order -= 1;
            insert_frame(&frame[BIT(upper_order)], upper_order);
        }
        frame->set_order(lower_order);
        return frame;
    }

    auto FrameBuddy::allocate(Order target_order) -> PmFrame * {
        for (auto order = target_order; order < kNumOrders; ++order) {
            if (!has_frame(order)) {
                continue;
            }

            auto *frame = get_frame(order);
            remove_frame(frame, order);
            acquire_inner(frame, target_order, order);
            return frame;
        }

        return nullptr;
    }

    auto FrameBuddy::release_inner(PmFrame *frame, Pfn pfn, Order order) -> void {
        // Coalesce upward. The true buddy is pfn ^ BIT(order); we must track the
        // lower of the two as the representative after each merge.
        while (order < kMaxOrder) {
            auto const buddy_pfn = pfn ^ BIT(order);
            auto *buddy = frame_of(buddy_pfn);
            if (!buddy || !is_mergeable(frame, buddy, order)) {
                break;
            }

            remove_frame(buddy, order);
            if (buddy_pfn < pfn) {
                frame = buddy;
                pfn = buddy_pfn;
            }
            order += 1;
        }

        insert_frame(frame, order);
    }

    auto FrameBuddy::release(PmFrame *frame, Order order) -> void {
        auto pfn = pfn_of(frame);
        auto const end_pfn = pfn + BIT(order);

        // Orders beyond the maximum are released in max-order chunks.
        order = ustl::algorithms::min(order, kMaxOrder);
        while (pfn != end_pfn) {
            release_inner(frame, pfn, order);
            pfn += BIT(order);
            frame = frame_of(pfn);
        }
    }

    auto FrameBuddy::free_count(Order order) const -> usize {
        usize count = 0;
        for (auto it = m_lists[order].begin(); it != m_lists[order].end(); ++it) {
            ++count;
        }
        return count;
    }

    auto FrameBuddy::empty(Order order) const -> bool {
        return m_lists[order].empty();
    }

    auto FrameBuddy::contains(PmFrame const *frame) const -> bool {
        return frame >= m_frames && frame < m_frames + m_count;
    }

} // namespace ours::mm::core
