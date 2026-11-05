/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ZONE_CANDIDATES_HPP
#define OURS_MM_PRIV_ZONE_CANDIDATES_HPP 1

#include <ours/mm/pm_zone.hpp>
#include <ours/mm/priv/alloc_request.hpp>

#include <ustl/views/span.hpp>

namespace ours::mm {
/// The zones of a node an allocation may draw from, in the order it should try
/// them: a range over a candidate list, narrowed by one request's criterion.
///
/// The candidate range holds eligibility filtering. Choosing a zone per
/// allocation used to live in a lambda inside
/// PmNode::get_frame_from_zone_list(), with the node filter written out there
/// and again in a walk that nothing on the allocation path used -- so a
/// question about a candidate's fitness had no home, which is why an admission
/// check against a zone's watermarks ended up inside the zone, the object that
/// owns the free lists, rather than beside the choice it informs.
///
/// The zone order and criterion are fixed for the allocation. The position
/// belongs to the Iterator that begin() hands out. The criterion is the
/// request's, and the request is still a separate value because it outlives
/// no walk and is shared by every layer -- what the range adds is the
/// walk, not the policy.
///
/// Three filters, in the order that rejects most cheaply first: a hole in the
/// by-type array, then the zone's type against the request's range, then its
/// node against the request's mask.
class ZoneCandidates {
    using Self = ZoneCandidates;
    using Zones = ustl::views::Span<PmZone *const>;

  public:
    /// One position in the walk: parked on a candidate that passed every
    /// filter, or past the last candidate.
    ///
    /// The position lives here rather than on the candidates so that the walk
    /// is an ordinary range: a caller writes `for (auto &zone: candidates)` and
    /// stops with `break`, which is what taking a frame from the first zone
    /// that can spare one looks like. The shape this replaces was a `next()`
    /// returning null as its sentinel -- a C iterator, where the end of the
    /// candidates and a zone that failed a filter were the same answer, and
    /// where walking the same candidates twice silently continued the first
    /// walk.
    class Iterator {
      public:
        FORCE_INLINE
        auto operator*() const -> PmZone & {
            return *m_candidates->m_zones[m_cursor];
        }

        FORCE_INLINE
        auto operator->() const -> PmZone * {
            return m_candidates->m_zones[m_cursor];
        }

        FORCE_INLINE
        auto operator++() -> Iterator & {
            m_cursor += 1;
            settle();
            return *this;
        }

        /// The cursor alone. Two iterators are only ever compared within one
        /// walk, so the range they came from is the same by construction.
        FORCE_INLINE
        auto operator==(Iterator const &other) const -> bool {
            return m_cursor == other.m_cursor;
        }

      private:
        friend class ZoneCandidates;

        Iterator(Self const &candidates, usize const cursor)
            : m_candidates(&candidates), m_cursor(cursor) {
            settle();
        }

        /// Moves the cursor forward to the first candidate the request admits,
        /// or to the end. Called on construction as well as on increment, so
        /// that an iterator is never parked on a rejected candidate and
        /// dereferencing one is always safe.
        auto settle() -> void {
            while (m_cursor < m_candidates->m_zones.size() &&
                   !m_candidates->admits(m_candidates->m_zones[m_cursor])) {
                m_cursor += 1;
            }
        }

        /// Borrowed: an iterator never outlives the range it walks, which
        /// in turn never outlives one allocation. A reference would do for the
        /// lifetime but not for the assignment an iterator owes its users.
        Self const *m_candidates;
        usize m_cursor;
    };

    /// |zones| is the candidate order -- a node's fallback order, or its own
    /// zones by type, whose holes are nulls this steps over.
    ZoneCandidates(Zones const zones, AllocRequest const &request)
        : m_zones(zones), m_request(request) {}

    [[nodiscard]]
    FORCE_INLINE
    auto begin() const -> Iterator {
        return Iterator{*this, 0};
    }

    [[nodiscard]]
    FORCE_INLINE
    auto end() const -> Iterator {
        return Iterator{*this, m_zones.size()};
    }

  private:
    /// Whether |zone| is a candidate this request may be served from.
    [[nodiscard]]
    auto admits(PmZone const *const zone) const -> bool {
        // A type this node has no zone of. The by-type array keeps a null per
        // missing type so that an index is a ZoneType; the fallback order has
        // none, so this costs one test there.
        if (!zone) {
            return false;
        }

        // Both bounds, unlike the walk this replaces, which read only the
        // upper one -- so a caller asking to keep off the low zones was
        // served from them anyway.
        auto const type = zone->zone_type();
        if (type < m_request.lowest_zone() || type > m_request.highest_zone()) {
            return false;
        }

        return m_request.allowed_nodes().test(zone->which_node());
    }

    Zones m_zones;

    /// The request snapshot belongs to this walk. The zone order is borrowed.
    AllocRequest m_request;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ZONE_CANDIDATES_HPP
