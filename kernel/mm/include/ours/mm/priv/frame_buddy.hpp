/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_FRAME_BUDDY_HPP
#define OURS_MM_PRIV_FRAME_BUDDY_HPP 1

#include <ours/mm/pm_frame.hpp>

#include <array>

#include <ustl/option.hpp>

namespace ours::mm {
/// Binary buddy allocator over the frames of one span of pfns: one free list
/// per order, and no lock of its own.
///
/// This is the seam that makes the physical-memory allocator host-testable:
/// the same algorithm runs in the kernel and in a host unit test, because
/// FrameBuddy depends on nothing global -- not the memmap, not a zone. What
/// it needs from its caller instead:
///
///   - The pfn of every block it is given to free. A descriptor does not say
///     where it is; the zone asks the memmap, a test knows.
///   - That the descriptors of the span's pfns in one max-order block --
///     2^MAX_ORDER pfns aligned to that size -- are consecutive elements of
///     one array.
///
/// The second is what lets it find a partner with no memmap: every block it
/// splits or merges lies inside one max-order block, and a partner outside
/// the span is never looked at, so the partner's descriptor is the one in
/// hand offset by the difference of their pfns, and that offset never leaves
/// the array. It never offsets further, nor subtracts two descriptors,
/// because nothing promises anything beyond: the sparse memmap allocates each
/// section's descriptors on their own, so the descriptors of a zone that
/// spans sections are several arrays, apart and in no particular order.
/// SparseMemMap pins a max-order block inside one section.
class FrameBuddy {
  public:
    using Order = usize;

    static constexpr Order NUM_ORDERS = NR_FRAME_ORDERS;
    static constexpr Order MAX_ORDER = MAX_FRAME_ORDER;

    /// A buddy for the pfns in [start_pfn, end_pfn), with nothing free until
    /// release() puts it there.
    FrameBuddy(Pfn start_pfn, Pfn end_pfn);

    /// A free block of exactly 2^|order| frames, split out of the lowest
    /// order that has one, or none when no order from |order| up does.
    ///
    /// An Option and not a pointer: "there is no block this large free" is an
    /// ordinary answer here, not an error and not an absent object, and the
    /// caller has to handle it on every call. A returned `PmFrame *` says the
    /// same thing with a value the type system also lets a caller dereference,
    /// which is how a null wrapped in `ustl::ok()` reached the frame-list
    /// loops two days ago (`4929d8a0`): every `if (result)` there was true
    /// because the test was is_ok(), not a test of the pointer. An Option's
    /// only way out is value(), which traps when there is nothing in it.
    [[nodiscard]]
    auto allocate(Order order) -> ustl::Option<PmFrame &>;

    /// Frees the block of 2^|order| frames whose first descriptor is |frame|
    /// and whose first pfn is |pfn|, merging it with its free partners up to
    /// MAX_ORDER. The block lies in the span and |pfn| is aligned to its size,
    /// as every buddy block is: its partner is found by flipping one bit.
    auto release(PmFrame *frame, Pfn pfn, Order order) -> void;

    auto free_count(Order order) const -> usize;
    auto empty(Order order) const -> bool;

  private:
    /// The descriptor of |other|, from |frame|, the descriptor of |pfn|. The
    /// two pfns lie in one max-order block, so their descriptors are in one
    /// array; see the class comment.
    FORCE_INLINE
    static auto frame_at(PmFrame *frame, Pfn pfn, Pfn other) -> PmFrame * {
        return frame + static_cast<isize>(other - pfn);
    }

    /// Whether |pfn| is in this buddy's span: the zone's, so a partner outside
    /// it is another zone's frame, or none.
    [[nodiscard]]
    FORCE_INLINE
    auto spans(Pfn pfn) const -> bool {
        return m_start_pfn <= pfn && pfn < m_end_pfn;
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

    /// Splits the block of 2^|upper_order| frames at |frame| down to
    /// 2^|lower_order|, putting the upper half of each split on its order's
    /// free list. |frame| is the lower half at every step, so it is the block
    /// the caller keeps; nothing is returned because the caller already holds
    /// it.
    auto acquire_inner(PmFrame *frame, Order lower_order, Order upper_order) -> void;

    Pfn m_start_pfn;
    Pfn m_end_pfn;
    std::array<PmFrame::List, NR_FRAME_ORDERS> m_lists;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_FRAME_BUDDY_HPP
