/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_PAGE_SPLICE_LIST_HPP
#define OURS_MM_VM_PAGE_SPLICE_LIST_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_page_list.hpp>
#include <ours/status.hpp>

#include <ktl/vec.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
/// A run of page slots on their way from one VMO to another, in order: what
/// VmObjectPaged::take_pages() fills and supply_pages() empties.
///
/// One slot for each page of the run, and each one of three things, as a slot
/// of a VmPageList is: a page, a zero marker, or nothing. Nothing here is a gap:
/// a page nobody wrote, which reads as zero where it came from. The three have
/// to keep their places, because the position in the run is the offset the
/// slot lands at: a list of pages alone could not say which offset a page
/// belongs to once a gap or a marker came before it.
///
/// Sparse: only the slots that hold something are stored, each with its index
/// in the run, and a gap is an index with nothing stored, so a run of a
/// thousand pages nobody wrote costs nothing. Filled once, in index order, and
/// emptied once, in index order (pop()).
///
/// It owns the pages it holds. A page appended and never popped is freed with
/// the list, so a supply that stops part way, or a list nobody supplies,
/// loses no page.
class VmPageSpliceList {
    using Self = VmPageSpliceList;

  public:
    using Slot = VmPageList::Slot;

    /// A run of |nr_pages| slots, every one a gap until something is appended.
    explicit VmPageSpliceList(usize nr_pages);
    ~VmPageSpliceList();
    USTL_NO_MOVEABLE_AND_COPYABLE(VmPageSpliceList);

    /// How many slots the run has, gaps included.
    [[nodiscard]]
    auto nr_pages() const -> usize {
        return m_nr_pages;
    }

    /// Whether nothing has been appended or popped: what take_pages() fills.
    [[nodiscard]]
    auto is_fresh() const -> bool {
        return m_held.empty() && m_position == 0;
    }

    /// Whether nothing has been popped yet: what supply_pages() empties.
    [[nodiscard]]
    auto is_unread() const -> bool {
        return m_position == 0;
    }

    /// Makes room for |nr_held| pages and zero markers, so that as many appends
    /// as that cannot fail. OutOfMem, and nothing changed, when there is none:
    /// a take asks for the room before it moves a single page, so that it
    /// either moves all of them or none.
    [[nodiscard]]
    auto reserve(usize nr_held) -> Status;

    /// Puts |page| at |index| of the run, which must be past every slot
    /// appended before it and inside the run. The list owns the page from
    /// here. OutOfMem when there is no room, and then the caller still does.
    [[nodiscard]]
    auto append_page(usize index, VmPage *page) -> Status;

    /// Puts a zero marker at |index| of the run, under the same rules.
    [[nodiscard]]
    auto append_zero_marker(usize index) -> Status;

    /// The slot at the next index of the run, and steps past it: a page, which
    /// is the caller's from here, a zero marker, or an empty Slot for a gap.
    /// Asked at most nr_pages() times.
    auto pop() -> Slot;

  private:
    struct Held {
        /// The slot's place in the run, from 0.
        usize index;
        Slot slot;
    };

    auto append(usize index, Slot slot) -> Status;

    /// The slots that hold something, in index order.
    ktl::Vec<Held> m_held;
    usize m_nr_pages;
    /// The index pop() hands out next.
    usize m_position = 0;
    /// The first of m_held that pop() has not handed out.
    usize m_next_held = 0;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_PAGE_SPLICE_LIST_HPP
