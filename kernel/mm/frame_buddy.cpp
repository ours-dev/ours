/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/priv/frame_buddy.hpp>

namespace ours::mm {
FrameBuddy::FrameBuddy(Pfn start_pfn, Pfn end_pfn)
    : m_start_pfn(start_pfn), m_end_pfn(end_pfn), m_lists() {}

auto FrameBuddy::acquire_inner(PmFrame *frame, Order lower_order, Order upper_order) -> void {
    // Split downward: at each step the freshly split upper half becomes the
    // buddy at that order. frame is always the lower half here, so pointer
    // arithmetic (&frame[1 << order]) is exactly the buddy, and it stays
    // inside the block being split, which is at most one max-order block.
    while (upper_order > lower_order) {
        upper_order -= 1;
        insert_frame(&frame[usize{1} << upper_order], upper_order);
    }
    frame->set_order(lower_order);
}

auto FrameBuddy::allocate(Order target_order) -> ustl::Option<PmFrame &> {
    for (auto order = target_order; order < NUM_ORDERS; ++order) {
        // front() already answers both halves of what has_frame() and
        // get_frame() used to answer in two steps, and it answers them
        // together: an empty list cannot hand out a head, so there is no
        // moment between the test and the read in which the two could
        // disagree.
        auto const head = m_lists[order].front();
        if (!head) {
            continue;
        }

        auto &frame = head.value();
        remove_frame(&frame, order);
        acquire_inner(&frame, target_order, order);
        return ustl::some_ref(frame);
    }

    return ustl::NONE;
}

auto FrameBuddy::release(PmFrame *frame, Pfn pfn, Order order) -> void {
    // In every build, as each is one comparison and each, broken, corrupts
    // the lists without a sound: an order past the top indexes past m_lists,
    // and a misaligned pfn flips its bit into the middle of some other block,
    // whose descriptor would then be unlinked as a free partner.
    ASSERT(order <= MAX_ORDER, "order {} is above the top order {}", order, MAX_ORDER);
    ASSERT((pfn & ((usize{1} << order) - 1)) == 0, "pfn {:#x} is not aligned to its order {}", pfn,
           order);
    DEBUG_ASSERT(spans(pfn) && pfn + (usize{1} << order) <= m_end_pfn,
                 "block [{:#x}, {:#x}) is outside the span [{:#x}, {:#x})", pfn,
                 pfn + (usize{1} << order), m_start_pfn, m_end_pfn);

    // Coalesce upward. The partner of the block at pfn is the block at
    // pfn ^ (1 << order), and after a merge the lower of the two stands for the
    // pair. Both lie in the aligned block of the next order up, and the
    // walk stops below MAX_ORDER, so every descriptor it reaches is inside the
    // max-order block |frame| is in: frame_at() never leaves its array.
    while (order < MAX_ORDER) {
        auto const buddy_pfn = pfn ^ (usize{1} << order);
        if (!spans(buddy_pfn)) {
            break;
        }
        auto *const buddy = frame_at(frame, pfn, buddy_pfn);
        if (!is_mergeable(frame, buddy, order)) {
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

} // namespace ours::mm
