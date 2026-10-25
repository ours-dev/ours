/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_PAGE_LIST_HPP
#define OURS_MM_VM_PAGE_LIST_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/status.hpp>

#include <utility>

#include <ustl/collections/btree.hpp>

namespace ours::mm {
/// The pages a VMO owns, keyed by page index within that object's range, and
/// the pages it knows to be zero without holding them (Slot).
///
/// This is what makes an offset stand for something: the second access to a
/// page has to find the page the first access installed. Without it a VMO is
/// not a store at all but a wrapper around the allocator, which is what this
/// one was -- every lookup allocated afresh and recorded nothing, so the same
/// offset yielded a different page each time. Eager mapping hid it, because a
/// range committed once is never asked for again; demand paging cannot, since
/// resolving a fault *is* asking a second time.
///
/// Keyed one entry per page. Keying the tree by *chunks* of slots instead --
/// 16, say, so that a tree node covers 16 pages and a sequential walk stays
/// inside one node -- is an amortisation missing here and worth having once
/// VMOs get large; recorded in docs/repairs/260903/mm.md.
///
/// The B-tree is the same one VmaSet and ProtectionRanges use. It replaces
/// ktl::xarray::XArray, which this class used to name: XArray's load(),
/// store(), erase() and make_entry() are declared and have never been
/// defined, so any call to them is a link error. That is precisely why this
/// class had no callers -- it could not have had any.
class VmPageList {
    using Self = VmPageList;

  public:
    /// Who owns the page in a slot: `Owned` pages belong to this VMO and are
    /// its to free, `Borrowed` ones are on loan from elsewhere.
    ///
    /// The values are chosen so the tag rides in the low bits of the page
    /// pointer -- PageFrame::ALIGNMENT is 64, so bits 0..5 of any frame
    /// descriptor address are clear -- and a slot stays a single word.
    enum class Tag {
        Owned,
        Borrowed = 0_bit,
    };

    /// What one slot holds: nothing, a page, or a zero marker.
    ///
    /// A zero marker says that the page at its index is all zeros, and holds
    /// no page. It is content, as a page is, and not an absence: a lookup
    /// through a tree of nodes stops at it instead of going on to the parent,
    /// whose page it hides, and a node with a page source does not ask the
    /// source for it, since the source already said what is there. A read of
    /// it is the shared zero page; a write gets a zeroed page that takes its
    /// place.
    ///
    /// One word: the page pointer with its Tag in bit 0, or ZERO_MARKER alone,
    /// which no page pointer can be, since a frame descriptor is aligned to 64.
    /// Zero is an empty slot. The list never stores one -- an empty slot is
    /// spelled by its absence from the tree -- but lookup() answers one for an
    /// index it holds nothing at.
    class Slot {
      public:
        constexpr Slot() = default;

        [[nodiscard]]
        auto is_empty() const -> bool {
            return m_word == 0;
        }

        [[nodiscard]]
        auto is_zero_marker() const -> bool {
            return m_word == ZERO_MARKER;
        }

        [[nodiscard]]
        auto is_page() const -> bool {
            return !is_empty() && !is_zero_marker();
        }

        /// The page, or nullptr for a slot that holds none: an empty one or a
        /// zero marker.
        [[nodiscard]]
        auto page() const -> VmPage * {
            if (!is_page()) {
                return nullptr;
            }
            return reinterpret_cast<VmPage *>(m_word & ~KIND_MASK);
        }

        /// Who owns the page. Asked only of a slot that holds one.
        [[nodiscard]]
        auto tag() const -> Tag {
            DEBUG_ASSERT(is_page(), "only a page has an owner");
            return static_cast<Tag>(m_word & static_cast<usize>(Tag::Borrowed));
        }

      private:
        friend class VmPageList;
        // A splice list holds slots taken out of one list on their way into
        // another, and makes them as the list does.
        friend class VmPageSpliceList;

        constexpr static usize ZERO_MARKER = 1_bit;
        constexpr static usize KIND_MASK = static_cast<usize>(Tag::Borrowed) | ZERO_MARKER;

        static_assert(PageFrame::ALIGNMENT > KIND_MASK,
                      "the tag and the marker have to fit in bits the frame descriptor's "
                      "alignment leaves clear");

        explicit constexpr Slot(usize const word) : m_word(word) {}

        static auto of_page(VmPage *const page, Tag const tag) -> Slot {
            return Slot(reinterpret_cast<usize>(page) | static_cast<usize>(tag));
        }

        constexpr static auto zero_marker() -> Slot {
            return Slot(ZERO_MARKER);
        }

        usize m_word = 0;
    };

    /// What the slot at |index| holds; an empty Slot when the list holds
    /// nothing there.
    ///
    /// An empty slot is not an error anywhere: for an anonymous VMO it is
    /// exactly the "nothing here yet" that the fault path answers by
    /// allocating.
    [[nodiscard]]
    auto lookup(PgOff const index) const -> Slot {
        auto const at = m_slots.find(index);
        if (at.is_none()) {
            return Slot();
        }
        return at.value()->second;
    }

    /// The page installed at |index|, or nullptr when the slot holds none:
    /// empty, or a zero marker.
    [[nodiscard]]
    auto get_page(PgOff const index) const -> VmPage * {
        return lookup(index).page();
    }

    /// How many pages the list holds, of either tag; a zero marker is no page.
    /// What a test asks to learn that a read allocated nothing, or that a write
    /// copied exactly one page; nothing in the kernel proper needs it.
    [[nodiscard]]
    auto nr_pages() const -> usize {
        return m_slots.size() - m_nr_zero_markers;
    }

    /// Whether any slot in [first, last) holds anything: a page, of either tag,
    /// or a zero marker.
    ///
    /// Asked by a clone, to tell whether the node being cloned has anything of
    /// its own in the range or only what it sees through its parent. A zero
    /// marker is the node's own: it hides what the parent has there.
    [[nodiscard]]
    auto has_content_in(PgOff const first, PgOff const last) const -> bool {
        auto const at = m_slots.lower_bound(first);
        if (at.is_none()) {
            return false;
        }
        return at.value()->first < last;
    }

    /// How many slots of [first, last) hold a page, by stepping from one held
    /// slot to the next: a cost in the slots held, not in the range's width.
    [[nodiscard]]
    auto nr_pages_in(PgOff const first, PgOff const last) const -> usize {
        usize count = 0;
        auto from = first;
        PgOff index = 0;
        while (from < last) {
            auto const slot = next_slot(from, &index);
            if (slot.is_empty() || index >= last) {
                break;
            }
            if (slot.is_page()) {
                ++count;
            }
            from = index + 1;
        }
        return count;
    }

    /// The first slot at or after |from| that holds anything, its index in
    /// |index|; an empty Slot when there is none. A step of a walk over the list
    /// that may take slots out as it goes, which an iterator would not
    /// survive: the caller asks again from the index after the one it just
    /// handled.
    [[nodiscard]]
    auto next_slot(PgOff const from, PgOff *const index) const -> Slot {
        auto const at = m_slots.lower_bound(from);
        if (at.is_none()) {
            return Slot();
        }
        *index = at.value()->first;
        return at.value()->second;
    }

    /// Takes the page at |index| out of the list and hands it back, or nullptr
    /// when the slot holds none; a zero marker there stays. Ownership goes with
    /// the page: the list no longer knows it, so it will not free it, and
    /// whoever asked now has to.
    ///
    /// A page moves between two lists by being inserted into the second before
    /// this is called on the first, so that a failed insertion leaves it where
    /// it was.
    auto remove_page(PgOff const index) -> VmPage * {
        auto const at = m_slots.find(index);
        if (at.is_none() || !at.value()->second.is_page()) {
            return nullptr;
        }
        auto *const page = at.value()->second.page();
        auto _ = m_slots.erase(index);
        return page;
    }

    /// Takes the zero marker at |index| out of the list. False, and nothing
    /// changed, when the slot holds none.
    [[nodiscard]]
    auto remove_zero_marker(PgOff const index) -> bool {
        auto const at = m_slots.find(index);
        if (at.is_none() || !at.value()->second.is_zero_marker()) {
            return false;
        }
        auto _ = m_slots.erase(index);
        m_nr_zero_markers -= 1;
        return true;
    }

    /// Takes every slot of |from|, leaving it empty. This list must be empty:
    /// a slot here would be dropped without its page being given back.
    ///
    /// A swap of the two trees, not a copy: no page moves and nothing is
    /// allocated, so it cannot fail half way. A clone puts a node's whole page
    /// store under a new hidden node this way, its zero markers with it.
    auto take_all_from(Self &from) -> void {
        DEBUG_ASSERT(m_slots.empty(), "the slots here would be lost");
        m_slots.swap(from.m_slots);
        std::swap(m_nr_zero_markers, from.m_nr_zero_markers);
    }

    /// Install |page| at |index|.
    ///
    /// Refuses to overwrite a slot that holds anything rather than replacing
    /// it. A page already there is reachable from every mapping of this VMO,
    /// so dropping it on the floor would leak it and strand whichever mapping
    /// still points at it; a zero marker there is content already, which only
    /// replace_zero_marker() may replace. Callers reach this only after a
    /// lookup has said the slot is empty, so AlreadyExists means a caller raced
    /// or miscounted -- both worth hearing about rather than papering over --
    /// or, for a page a source supplies, that the slot was supplied before.
    ///
    /// Returns OutOfMem when the tree cannot grow; the caller still owns
    /// |page| in that case and must give it back.
    auto insert_page(PgOff const index, VmPage *const page, Tag const tag) -> Status {
        DEBUG_ASSERT(page, "an empty slot is spelled by absence, not by a null page");
        return insert(index, Slot::of_page(page, tag));
    }

    /// Records that the page at |index| is zero, with no page: a zero marker.
    /// Refuses a slot that holds anything, as insert_page() does, and answers
    /// OutOfMem when the tree cannot grow.
    auto insert_zero_marker(PgOff const index) -> Status {
        auto const status = insert(index, Slot::zero_marker());
        if (Status::Ok == status) {
            m_nr_zero_markers += 1;
        }
        return status;
    }

    /// Puts |page| in the place of the zero marker at |index|, which must be
    /// there. The slot already exists, so nothing is allocated and nothing can
    /// fail: what a write to a marker does once it has its zeroed page.
    auto replace_zero_marker(PgOff const index, VmPage *const page, Tag const tag) -> void {
        DEBUG_ASSERT(page, "a marker is replaced by a page, not by nothing");
        auto const at = m_slots.find(index);
        DEBUG_ASSERT(at.is_some() && at.value()->second.is_zero_marker(),
                     "no zero marker at index {} to replace", index);
        at.value()->second = Slot::of_page(page, tag);
        m_nr_zero_markers -= 1;
    }

    /// Puts a zero marker in the place of the page at |index|, which must be
    /// there, and hands the page back, the caller's from here. The inverse of
    /// replace_zero_marker(), and like it, it allocates nothing and cannot
    /// fail: what taking a page out of a node does when the slot has to keep
    /// hiding a page further up the tree.
    auto replace_page_with_zero_marker(PgOff const index) -> VmPage * {
        auto const at = m_slots.find(index);
        DEBUG_ASSERT(at.is_some() && at.value()->second.is_page(), "no page at index {} to replace",
                     index);
        auto *const page = at.value()->second.page();
        at.value()->second = Slot::zero_marker();
        m_nr_zero_markers += 1;
        return page;
    }

    /// Hands every page this list *owns* to |visitor| and then empties the list,
    /// zero markers included.
    ///
    /// Owned only: a Borrowed slot is a page on loan from elsewhere, and its
    /// lender is the one that frees it. Emptying regardless is deliberate --
    /// after this the list describes nothing, which is the only honest state
    /// for a store whose pages have been given away.
    ///
    /// The visitor takes the page rather than this class calling the allocator
    /// itself, so that this header stays a container and does not have to
    /// include the page allocator to describe how a list is torn down.
    template <typename Visitor>
    auto drain_owned(Visitor &&visitor) -> void {
        for (auto &entry: m_slots) {
            auto const slot = entry.second;
            if (slot.is_page() && slot.tag() == Tag::Owned) {
                visitor(slot.page());
            }
        }
        m_slots.clear();
        m_nr_zero_markers = 0;
    }

  private:
    auto insert(PgOff const index, Slot const slot) -> Status {
        auto inserted = m_slots.try_emplace(index, slot);
        if (inserted.is_err()) {
            return Status::OutOfMem;
        }
        if (!inserted.unwrap().second) {
            return Status::AlreadyExists;
        }
        return Status::Ok;
    }

    ustl::collections::BTreeMap<PgOff, Slot> m_slots;

    /// How many of the slots hold a zero marker, so that nr_pages() need not
    /// walk them. Changed wherever a marker comes or goes, and nowhere else.
    usize m_nr_zero_markers = 0;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_PAGE_LIST_HPP
