/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PROTECTION_RANGES_HPP
#define OURS_MM_PROTECTION_RANGES_HPP 1

#include <ours/mm/types.hpp>
#include <ours/status.hpp>

#include <algorithm>
#include <functional>

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/btree.hpp>

namespace ours::mm {
/// Tracks which MMU protection flags govern each sub-range of one VmMapping.
///
/// The tracked window is fixed by init(); update() rewrites the flags of a
/// sub-range of it, splitting and coalescing the underlying regions as
/// needed. A region carrying no permission at all is the canonical
/// representation of "unmapped": unmap() produces zero-permission regions and
/// protect() treats them that way.
///
/// The regions always tile the window -- ordered by base, disjoint, touching,
/// and merged wherever two neighbours agree on their flags -- so enumerating
/// any range within the window never observes a gap, and every address is
/// covered by exactly one region's flags.
class ProtectionRanges {
  public:
    class Enumerator;

    /// Claim the window [base, base + size) and give it |mmuf| throughout.
    auto init(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    /// Set the flags of [base, base + size) to |mmuf|. The range must fall
    /// inside the window claimed by init(). Boundary regions are split and
    /// equal-flag neighbours merged, so the region count stays proportional
    /// to the number of distinct flag regimes, not to the number of updates.
    /// On OutOfMem at most one benign boundary split may linger: it changes
    /// no flag and the next update coalesces it away.
    auto update(VirtAddr base, usize size, MmuFlags mmuf) -> Status;

    /// Make an Enumerator over the regions intersecting [base, base + size),
    /// each clipped to the range. The Enumerator is transient: an update
    /// rebalances the tree it walks.
    [[nodiscard]]
    auto make_enumerator(VirtAddr base, usize size) const -> Enumerator;

    /// Check the tiling invariants. A DEBUG_ASSERT walk, so it costs nothing
    /// in release builds; update() and init() run it on their way out.
    auto validate() const -> void;

  private:
    /// One flag regime. Both bounds are explicit -- the left one too -- so
    /// no invariant rests on how the container orders the regions.
    struct Region {
        VirtAddr base;
        VirtAddr end;
        MmuFlags mmuf;
    };

    struct RegionBaseOf final {
        [[nodiscard]]
        auto operator()(Region const &region) const noexcept -> VirtAddr {
            return region.base;
        }
    };

    /// Regions keyed by base, allocated through the default ustl resource the
    /// same way VmaSet's B-tree allocates its nodes.
    using RegionTree = ustl::collections::BTree<VirtAddr, Region, RegionBaseOf, std::less<>,
                                                ustl::collections::Allocator<Region>>;

    /// Iterator to the region containing |addr|, which always exists for an
    /// in-window |addr| because the regions tile it.
    auto region_containing(VirtAddr addr) -> RegionTree::iterator;
    auto region_containing(VirtAddr addr) const -> RegionTree::const_iterator;

    /// The window claimed by init().
    VirtAddr m_base;
    VirtAddr m_end;

    /// Keyed by base, disjoint, tiling [m_base, m_end), coalesced by flags.
    RegionTree m_regions;
};

/// Walks the regions intersecting one range, handing each out clipped to it.
class ProtectionRanges::Enumerator {
    using Self = ProtectionRanges::Enumerator;

  public:
    /// What next() hands back: whether the walk produced another region, and
    /// the region itself. Kept as the same plain aggregate the previous
    /// implementation used, so consumers keep decomposing it in the condition
    /// of their `while` (P0963R3).
    struct NextResult {
        bool valid;
        VirtAddr base;
        usize size;
        MmuFlags mmuf;

        explicit operator bool() const {
            return valid;
        }
    };

    FORCE_INLINE
    auto next() -> NextResult {
        if (m_cursor == m_last) {
            return {};
        }
        Region const &region = *m_cursor++;
        // Every region between m_cursor and m_last intersects the range, so
        // the clip below always leaves a non-empty span.
        VirtAddr const base = std::max(region.base, m_lower);
        VirtAddr const end = std::min(region.end, m_upper);
        return {true, base, end - base, region.mmuf};
    }

  private:
    friend class ProtectionRanges;

    Enumerator(RegionTree::const_iterator first, RegionTree::const_iterator last, VirtAddr lower,
               VirtAddr upper)
        : m_cursor(first), m_last(last), m_lower(lower), m_upper(upper) {}

    RegionTree::const_iterator m_cursor;
    RegionTree::const_iterator m_last;
    VirtAddr m_lower;
    VirtAddr m_upper;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PROTECTION_RANGES_HPP
