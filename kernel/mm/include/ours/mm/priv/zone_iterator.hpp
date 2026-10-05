/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ZONE_ITERATOR_HPP
#define OURS_MM_PRIV_ZONE_ITERATOR_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/node_mask.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>

/// The filtered walk over a node's zones that the allocator's zone selection
/// is meant to be built on. Not used by the allocation path yet --
/// PmNode::get_frame_from_zone_list() walks the fallback order directly -- and
/// kept here, rather than inside pm_node.cpp, so that pmnode.test
/// (kernel/commands/pm_node_test.cpp) can check it before anything relies on
/// it.
namespace ours::mm {
/// Walks a list of zones, yielding only the ones that pass the filter: on one
/// of |nodes|, and of a type no higher than the upper bound.
///
/// Rewritten along with ZoneList. The old move_next() did not filter at all:
/// a zone failing a test only advanced the cursor one extra step, and the
/// function then returned whatever zone it had landed on, filtered or not. It
/// also could not walk the by-type array, whose holes are nulls it would have
/// dereferenced.
struct ZoneIterator {
    using Self = ZoneIterator;
    using ZoneRef = ZoneList::ZoneRef;

    auto set_nodes(NodeMask const &nm) -> Self & {
        nodes = &nm;
        return *this;
    }

    auto set_zone_range(ZoneType lower, ZoneType upper) -> Self & {
        lower_zone_type = lower;
        upper_zone_type = upper;
        return *this;
    }

    auto set_zones(ZoneList::Zones const list) -> Self & {
        zones = list;
        next = 0;
        return *this;
    }

    auto move_next() -> ZoneRef;

    ZoneList::Zones zones;
    usize next = 0;
    NodeMask const *nodes = nullptr;
    ZoneType lower_zone_type; // Reserved and not used now.
    ZoneType upper_zone_type;
};

FORCE_INLINE
inline auto ZoneIterator::move_next() -> ZoneRef {
    while (next < zones.size()) {
        auto *const zone = zones[next++];
        if (!zone) {
            continue; // A type this node has no zone of.
        }
        if (zone->zone_type() > upper_zone_type) {
            continue;
        }
        if (nodes && !nodes->test(zone->which_node())) {
            continue;
        }
        return zone;
    }
    return ZoneRef();
}

/// CPU's and Thread's capacity on nodes.
///
/// 1. The former storaged in PmNode::Current is an optimized occasion.
/// 2. The latter storaged in MemoryAllocationState is a hard constrict.

struct AllocationContext {
    auto build(PmNode *node, Gaf gaf, usize order, NodeMask const &nodemask) -> Status {
        // A Gaf names no zone, so every allocation may use zones up to
        // Normal. It used to be read from Gaf's Dma, Dma32 and Movable bits,
        // which nothing ever passed.
        ziter.set_nodes(nodemask).set_zone_range(ZoneType::Dma, ZoneType::Normal);

        // This node's zones only, for Gaf::ThisNode -- the list
        // SharedSequential was presumably reserved for -- or the whole
        // fallback order.
        auto const *const list = node->zone_list();
        if (!!(gaf & Gaf::ThisNode)) {
            ziter.set_zones(list->local_zones());
        } else {
            ziter.set_zones(list->fallback());
        }

        return Status::Ok;
    }

    ZoneIterator ziter;

    NodeMask m_nodes;
};

} // namespace ours::mm

#endif // OURS_MM_PRIV_ZONE_ITERATOR_HPP
