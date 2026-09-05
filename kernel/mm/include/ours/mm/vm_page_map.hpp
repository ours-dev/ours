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
/// Keyed one entry per page. Zircon's counterpart, VmPageList, keys its WAVL
/// tree by *chunks* of 16 slots (VmPageListNode) so a tree node covers 16
/// pages and a sequential walk stays inside one node; that amortisation is
/// missing here and is worth having once VMOs get large -- recorded in
/// docs/repairs/260903/mm.md.
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
    /// descriptor address are clear -- and a slot stays a single word. Zircon
    /// packs VmPageOrMarker's type into the low bits of the same word for the
    /// same reason.
    enum class Tag {
        Owned,
        Borrowed = BIT(0),
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

    ustl::collections::BTreeMap<PgOff, Slot> m_slots;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_PAGE_MAP_HPP
