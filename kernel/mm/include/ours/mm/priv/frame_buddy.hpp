/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_FRAME_BUDDY_HPP
#define OURS_MM_PRIV_FRAME_BUDDY_HPP 1

#include <ours/mm/pm_frame.hpp>

#include <array>

namespace ours::mm {
/// Pure binary buddy allocator over a caller-provided array of PmFrame.
/// This is the seam that makes the physical-memory allocator host-testable:
/// the same algorithm runs in the kernel and in a host unit test, because
/// FrameBuddy depends on nothing global.
class FrameBuddy {
  public:
    using Order = usize;

    static constexpr Order NUM_ORDERS = NR_FRAME_ORDERS;
    static constexpr Order MAX_ORDER = MAX_FRAME_ORDER;

    FrameBuddy() = default;
    FrameBuddy(PmFrame *frames, usize count, Pfn base_pfn);
    auto attach(PmFrame *frames, usize count, Pfn base_pfn) -> void;

    /// Whether attach(), or the constructor taking frames, has given this
    /// buddy its frame array: a default-constructed one has none.
    [[nodiscard]]
    auto is_attached() const -> bool {
        return m_frames != nullptr;
    }

    auto allocate(Order order) -> PmFrame *;
    auto release(PmFrame *frame, Order order) -> void;

    auto free_count(Order order) const -> usize;
    auto empty(Order order) const -> bool;
    auto contains(PmFrame const *frame) const -> bool;

  private:
    FORCE_INLINE
    auto pfn_of(PmFrame const *frame) const -> Pfn {
        return m_base_pfn + static_cast<usize>(frame - m_frames);
    }

    FORCE_INLINE
    auto frame_of(Pfn pfn) const -> PmFrame * {
        if (pfn < m_base_pfn) {
            return nullptr;
        }
        auto const index = pfn - m_base_pfn;
        return index < m_count ? &m_frames[index] : nullptr;
    }

    FORCE_INLINE
    auto is_mergeable(PmFrame const *self, PmFrame const *buddy, Order order) const -> bool {
        if (buddy->role() != PageFrame::Role::Pmm) {
            return false;
        }
        if (buddy->order() != order) {
            return false;
        }
        if (self->zone() != buddy->zone()) {
            return false;
        }
        if (self->nid() != buddy->nid()) {
            return false;
        }
        return true;
    }

    FORCE_INLINE
    auto has_frame(Order order) const -> bool {
        return !m_lists[order].empty();
    }

    FORCE_INLINE
    auto get_frame(Order order) -> PmFrame * {
        return &m_lists[order].front().value();
    }

    FORCE_INLINE
    auto remove_frame(PmFrame *frame, Order order) -> void {
        auto const to_erase = m_lists[order].iterator_to(*frame);
        auto _ = m_lists[order].erase(to_erase);
        frame->set_role(PageFrame::Role::None);
    }

    FORCE_INLINE
    auto insert_frame(PmFrame *frame, Order order) -> void {
        m_lists[order].push_back(*frame);
        frame->set_role(PageFrame::Role::Pmm);
        frame->set_order(order);
    }

    auto acquire_inner(PmFrame *frame, Order lower_order, Order upper_order) -> PmFrame *;
    auto release_inner(PmFrame *frame, Pfn pfn, Order order) -> void;

    PmFrame *m_frames = nullptr;
    usize m_count = 0;
    Pfn m_base_pfn = 0;
    std::array<PmFrame::List, NR_FRAME_ORDERS> m_lists;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_FRAME_BUDDY_HPP
