/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_PAGE_MAP_HPP
#define OURS_MM_VM_PAGE_MAP_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/status.hpp>

#include <ustl/collections/btree.hpp>

namespace ours::mm {
/// The pages a VMO owns, keyed by page index within that object's range.
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
class VmPageMap {
    using Self = VmPageMap;

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

    /// The page installed at |index|, or nullptr when the slot is empty.
    ///
    /// An empty slot is not an error anywhere: for an anonymous VMO it is
    /// exactly the "nothing here yet" that the fault path answers by
    /// allocating.
    [[nodiscard]]
    auto get_page(PgOff index) const -> VmPage * {
        auto const slot = m_slots.find(index);
        if (slot.is_none()) {
            return nullptr;
        }
        return slot_page(slot.value()->second);
    }

    /// How many pages the map holds, of either tag. What a test asks to learn
    /// that a read allocated nothing, or that a write copied exactly one page;
    /// nothing in the kernel proper needs it.
    [[nodiscard]]
    auto size() const -> usize {
        return m_slots.size();
    }

    /// Whether any slot in [first, last) holds a page, of either tag.
    ///
    /// Asked by a clone, to tell whether the node being cloned has anything of
    /// its own in the range or only what it sees through its parent.
    [[nodiscard]]
    auto has_page_in(PgOff first, PgOff last) const -> bool {
        auto const at = m_slots.lower_bound(first);
        if (at.is_none()) {
            return false;
        }
        return at.value()->first < last;
    }

    /// The first page at or after |from|, its index in |index|, or nullptr when
    /// there is none. A step of a walk over the map that may take pages out as
    /// it goes, which an iterator would not survive: the caller asks again from
    /// the index after the one it just handled.
    [[nodiscard]]
    auto next_page(PgOff from, PgOff *index) const -> VmPage * {
        auto const at = m_slots.lower_bound(from);
        if (at.is_none()) {
            return nullptr;
        }
        *index = at.value()->first;
        return slot_page(at.value()->second);
    }

    /// Takes the page at |index| out of the map and hands it back, or nullptr
    /// when the slot is empty. Ownership goes with it: the map no longer knows
    /// the page, so it will not free it, and whoever asked now has to.
    ///
    /// A page moves between two maps by being inserted into the second before
    /// this is called on the first, so that a failed insertion leaves it where
    /// it was.
    auto remove_page(PgOff index) -> VmPage * {
        auto const slot = m_slots.find(index);
        if (slot.is_none()) {
            return nullptr;
        }
        auto *const page = slot_page(slot.value()->second);
        auto _ = m_slots.erase(index);
        return page;
    }

    /// Takes every slot of |from|, leaving it empty. This map must be empty:
    /// a slot here would be dropped without its page being given back.
    ///
    /// A swap of the two trees, not a copy: no page moves and nothing is
    /// allocated, so it cannot fail half way. A clone puts a node's whole page
    /// store under a new hidden node this way.
    auto take_all_from(Self &from) -> void {
        DEBUG_ASSERT(m_slots.empty(), "the slots here would be lost");
        m_slots.swap(from.m_slots);
    }

    /// Install |page| at |index|.
    ///
    /// Refuses to overwrite an occupied slot rather than replacing it: the
    /// page already there is reachable from every mapping of this VMO, so
    /// dropping it on the floor would leak it and strand whichever mapping
    /// still points at it. Callers reach this only after get_page() has said
    /// the slot is empty, so AlreadyExists means a caller raced or miscounted
    /// -- both worth hearing about rather than papering over.
    ///
    /// Returns OutOfMem when the tree cannot grow; the caller still owns
    /// |page| in that case and must give it back.
    auto insert_page(PgOff index, VmPage *page, Tag tag) -> Status {
        DEBUG_ASSERT(page, "an empty slot is spelled by absence, not by a null page");

        auto inserted = m_slots.try_emplace(index, make_slot(page, tag));
        if (inserted.is_err()) {
            return Status::OutOfMem;
        }
        if (!inserted.unwrap().second) {
            return Status::AlreadyExists;
        }
        return Status::Ok;
    }

    /// Hands every page this map *owns* to |visitor| and then empties the map.
    ///
    /// Owned only: a Borrowed slot is a page on loan from elsewhere, and its
    /// lender is the one that frees it. Emptying regardless is deliberate --
    /// after this the map describes nothing, which is the only honest state
    /// for a store whose pages have been given away.
    ///
    /// The visitor takes the page rather than this class calling the allocator
    /// itself, so that this header stays a container and does not have to
    /// include the page allocator to describe how a map is torn down.
    template <typename Visitor>
    auto drain_owned(Visitor &&visitor) -> void {
        for (auto &entry: m_slots) {
            auto const slot = entry.second;
            if (slot_tag(slot) == Tag::Owned) {
                visitor(slot_page(slot));
            }
        }
        m_slots.clear();
    }

  private:
    /// A slot is the page pointer with its Tag folded into the low bits.
    using Slot = usize;

    constexpr static Slot TAG_MASK = static_cast<Slot>(Tag::Borrowed);

    static_assert(PageFrame::ALIGNMENT > TAG_MASK,
                  "the tag has to fit in bits the frame descriptor's alignment leaves clear");

    FORCE_INLINE
    static auto make_slot(VmPage *page, Tag tag) -> Slot {
        return reinterpret_cast<Slot>(page) | static_cast<Slot>(tag);
    }

    FORCE_INLINE
    static auto slot_page(Slot slot) -> VmPage * {
        return reinterpret_cast<VmPage *>(slot & ~TAG_MASK);
    }

    FORCE_INLINE
    static auto slot_tag(Slot slot) -> Tag {
        return static_cast<Tag>(slot & TAG_MASK);
    }

    ustl::collections::BTreeMap<PgOff, Slot> m_slots;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_PAGE_MAP_HPP
