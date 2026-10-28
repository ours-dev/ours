/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ZONE_SELECTOR_HPP
#define OURS_MM_PRIV_ZONE_SELECTOR_HPP 1

#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/priv/alloc_request.hpp>

namespace ours::mm {
/// Which of a node's candidate zones an allocation may draw from, one at a
/// time.
///
/// The selector is the object the allocator was missing. Choosing a zone per
/// allocation used to live in a lambda inside
/// PmNode::get_frame_from_zone_list(), with the node filter written out there
/// and again in a walk that nothing on the allocation path used -- so a
/// question about a candidate's fitness had no home, which is why an admission
/// check against a zone's watermarks ended up inside the zone, the object that
/// owns the free lists, rather than beside the choice it informs.
///
/// It holds the cursor, which is why it is not part of AllocRequest: the
/// request is an immutable value one thread owns for the length of one
/// allocation, and a mutable iteration position inside it is what made the
/// shape this replaces hard to reason about. A selector is a stack object with
/// the same lifetime, borrowing the request.
///
/// Three filters, in the order that rejects most cheaply first: a hole in the
/// by-type array, then the zone's type against the request's range, then its
/// node against the request's mask.
class ZoneSelector {
    using Self = ZoneSelector;

  public:
    /// |zones| is the candidate order -- a node's fallback order, or its own
    /// zones by type, whose holes are nulls this steps over. |request| must
    /// outlive the selector.
    ZoneSelector(ZoneList::Zones const zones, AllocRequest const &request)
        : m_zones(zones), m_request(&request), m_cursor(0) {}

    /// The next zone that passes every filter, or null once the candidates run
    /// out. Advancing is the only state it has.
    auto next() -> PmZone * {
        while (m_cursor < m_zones.size()) {
            auto *const zone = m_zones[m_cursor++];

            // A type this node has no zone of. The by-type array keeps a null
            // per missing type so that an index is a ZoneType; the fallback
            // order has none, so this costs one test there.
            if (!zone) {
                continue;
            }

            // Both bounds, which is new: the walk this replaces read only the
            // upper one, so a caller asking to keep off the low zones was
            // served from them anyway.
            auto const type = zone->zone_type();
            if (type < m_request->lowest_zone || type > m_request->highest_zone) {
                continue;
            }

            if (!m_request->allowed_nodes.test(zone->which_node())) {
                continue;
            }

            return zone;
        }
        return nullptr;
    }

  private:
    ZoneList::Zones m_zones;
    AllocRequest const *m_request;
    usize m_cursor;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ZONE_SELECTOR_HPP
