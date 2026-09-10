#include <ours/assert.hpp>
#include <ours/mm/protection_ranges.hpp>

#include <ustl/mem/align.hpp>

namespace ours::mm {
auto ProtectionRanges::init(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    if (!size) {
        return Status::InvArgs;
    }
    DEBUG_ASSERT(ustl::mm::is_aligned(base, PAGE_SIZE));
    DEBUG_ASSERT(ustl::mm::is_aligned(size, PAGE_SIZE));

    m_base = base;
    m_end = base + size;

    auto inserted = m_regions.insert(Region{m_base, m_end, mmuf});
    if (inserted.is_err()) {
        return Status::OutOfMem;
    }
    DEBUG_ASSERT(inserted.unwrap().second, "the initial region already exists");

    return Status::Ok;
}

auto ProtectionRanges::update(VirtAddr base, usize size, MmuFlags mmuf) -> Status {
    DEBUG_ASSERT(!m_regions.empty(), "update before init");

    VirtAddr const end = base + size;
    if (!size || base < m_base || end > m_end) {
        return Status::InvArgs;
    }

    // Detach the partial overlaps at the boundaries of the range: a boundary
    // region is split only when it reaches past the range with the wrong
    // flags, and the split-off tail keeps the old regime. The tree rebalances
    // under insert, so no step mutates through an iterator that survived one
    // -- every mutation re-locates its region by key.
    auto b = region_containing(end - 1);
    if (b->end > end && b->mmuf != mmuf) {
        auto const b_base = b->base;
        auto split = m_regions.insert(Region{end, b->end, b->mmuf});
        if (split.is_err()) {
            // Nothing destructive has happened yet, so the range keeps its
            // old flags wholesale.
            return Status::OutOfMem;
        }
        DEBUG_ASSERT(split.unwrap().second, "the end boundary already exists");
        m_regions.find(b_base).value()->end = end;
    }
    auto a = region_containing(base);
    if (a->base < base && a->mmuf != mmuf) {
        auto const a_base = a->base;
        auto split = m_regions.insert(Region{base, a->end, a->mmuf});
        if (split.is_err()) {
            // At worst the boundary split above lingers; it changed no flag
            // and the next update coalesces it away.
            return Status::OutOfMem;
        }
        DEBUG_ASSERT(split.unwrap().second, "the base boundary already exists");
        // Re-locate by the original region's key: the split piece keyed at
        // |base| now contains |base| itself, and truncating that one would
        // leave an empty region behind.
        m_regions.find(a_base).value()->end = base;
    }

    // Overwrite every region intersecting the range. A region reaching past
    // an endpoint can only still do so when it already carried |mmuf| -- the
    // alternative was split off above -- so this never clobbers a tail that
    // was meant to keep its old flags. Pure value mutation, no tree change,
    // so the walk needs no re-location.
    for (auto iter = region_containing(base); iter != m_regions.end() && iter->base < end; ++iter) {
        iter->mmuf = mmuf;
    }

    // Merge equal-flag neighbours; only regions around the updated range can
    // newly qualify, but a whole-window sweep is the same code with no case
    // analysis about which ends moved. The walk is carried by the running
    // base address rather than an iterator, because erasing rebalances the
    // tree under it.
    VirtAddr scan = m_base;
    for (;;) {
        auto current = m_regions.lower_bound(scan);
        if (current.is_none()) {
            break;
        }
        auto next = current.value();
        ++next;
        if (next == m_regions.end()) {
            break;
        }
        if (current.value()->mmuf != next->mmuf) {
            scan = next->base;
            continue;
        }

        VirtAddr const merged_end = next->end;
        m_regions.erase(next->base);
        region_containing(scan)->end = merged_end;
    }

    validate();
    return Status::Ok;
}

auto ProtectionRanges::make_enumerator(VirtAddr base, usize size) const -> Enumerator {
    DEBUG_ASSERT(!m_regions.empty(), "make_enumerator before init");
    DEBUG_ASSERT(base >= m_base && base + size <= m_end, "range escapes the window");

    auto const end = base + size;
    auto last = region_containing(base);
    auto first = last;
    while (last != m_regions.end() && last->base < end) {
        ++last;
    }

    return Enumerator{first, last, base, end};
}

auto ProtectionRanges::validate() const -> void {
    if (m_regions.empty()) {
        return;
    }

    auto current = m_regions.begin();
    DEBUG_ASSERT(current->base == m_base, "regions do not reach the window start");
    for (; current != m_regions.end(); ++current) {
        DEBUG_ASSERT(current->base < current->end, "empty region");
        if (current != m_regions.begin()) {
            auto previous = current;
            --previous;
            DEBUG_ASSERT(previous->end == current->base, "regions overlap or leave a gap");
            DEBUG_ASSERT(previous->mmuf != current->mmuf, "uncoalesced equal-flag neighbours");
        }
    }
    --current;
    DEBUG_ASSERT(current->end == m_end, "regions do not reach the window end");
}

auto ProtectionRanges::region_containing(VirtAddr addr) -> RegionTree::iterator {
    auto successor = m_regions.upper_bound(addr);
    if (successor.is_none()) {
        auto last = m_regions.end();
        --last;
        return last;
    }

    auto region = successor.value();
    --region;
    return region;
}

auto ProtectionRanges::region_containing(VirtAddr addr) const -> RegionTree::const_iterator {
    auto successor = m_regions.upper_bound(addr);
    if (successor.is_none()) {
        auto last = m_regions.end();
        --last;
        return last;
    }

    auto region = successor.value();
    --region;
    return region;
}

} // namespace ours::mm
