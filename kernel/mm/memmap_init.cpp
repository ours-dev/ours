/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/memmap_init.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

namespace ours::mm {
namespace {
    /// The frames in a hole. They get a descriptor too -- marked reserved, so
    /// nothing hands them out -- because a pfn inside a zone's span has to
    /// resolve to something.
    ///
    /// Unlike the range below, a hole may genuinely have no descriptor behind
    /// it, so this asks pfn_valid() first instead of asserting.
    INIT_CODE auto init_memmap_reserved(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NnId nid)
        -> void {
        for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
            if (!pfn_valid(pfn)) {
                continue;
            }

            auto frame = pfn_to_frame(pfn);
            frame->init(ztype, pfn_to_secnum(pfn), nid);
            frame->mark_reserved();
        }
    }

    INIT_CODE auto init_memmap_range(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NnId nid) -> void {
        for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
            auto frame = pfn_to_frame(pfn);
            DEBUG_ASSERT(frame, "A usable pfn with no descriptor behind it");

            frame->init(ztype, pfn_to_secnum(pfn), nid);
        }
    }

    /// Descriptors for the overlap between [start, end) and the part of `zone`
    /// that is really there. Returns the first pfn it did not initialise.
    INIT_CODE auto init_memmap_zone(ZoneType zone, Pfn zone_start, Pfn zone_end, Pfn start, Pfn end,
                                    Pfn hole_start, NnId nid) -> Pfn {
        auto const first = std::clamp(start, zone_start, zone_end);
        auto const last = std::clamp(end, zone_start, zone_end);

        if (first >= last) {
            // This zone does not reach the part of the region that is left, so
            // nothing was initialised and the caller's cursor has not moved.
            //
            // `start`, not `first`: the clamped value is a *zone* boundary and
            // says nothing about the region. Returning it walked the cursor
            // backwards for every region that lies above the zone -- the DMA
            // pass over a region at 0x1EF02 handed back 0x1000, and the DMA32
            // pass that followed then re-initialised everything from 0x1000 up,
            // which is what tripped "Duplicated to initialize a frame".
            return start;
        }
        init_memmap_range(first, last, zone, nid);

        if (hole_start < first) {
            init_memmap_reserved(hole_start, first, zone, nid);
        }
        return last;
    }
} // namespace

INIT_CODE auto build_memmap() -> Status {
    auto &model = MemMap::get();
    if (auto const status = model.init(); Status::Ok != status) {
        return status;
    }

    // Every usable range, in one pass. The boot allocator already stamps each
    // region with the node it belongs to, so `region.node` says everything the
    // node-by-node walk this used to do was asking PmNode for.
    //
    // Dropping that walk is what lets this run *before* PmNode exists, and that
    // ordering is the point: a PmZone binds its descriptor run while it is being
    // built, so the descriptors have to be there first. Linux draws the line in
    // the same place -- sparse_init() reads memblock directly and never touches
    // a pg_data_t, which is why free_area_init() can call it ahead of the
    // per-node loop.
    auto status = Status::Ok;
    auto const visit_status = EarlyMem::visit_usable(
        EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
        [&](bootmem::MemoryRegion const region) {
            status = model.add_range(region.range.base, region.range.end(), region.node);
            return status == Status::Ok;
        });
    if (visit_status != Status::Ok && status == Status::Ok) {
        status = visit_status;
    }
    return status;
}

INIT_CODE auto init_memmap() -> void {
    Pfn hole_start_pfn = 0, start_pfn = 0, end_pfn = 0;
    [[maybe_unused]]
    auto const status = EarlyMem::visit_usable(
        EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
        [&](bootmem::MemoryRegion const region) {
            auto const node = PmNode::node(region.node);
            DEBUG_ASSERT(node != nullptr, "");

            start_pfn = phys_to_pfn(region.range.base);
            end_pfn = phys_to_pfn(region.range.end());
            for (auto zone: node->zone_queues()->get_queue(ZoneQueues::LocalContiguous)) {
                start_pfn = init_memmap_zone(zone->zone_type(), zone->start_pfn(), zone->end_pfn(),
                                             start_pfn, end_pfn, hole_start_pfn, region.node);
                hole_start_pfn = end_pfn;
            }
            return true;
        });
    DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate early memory");

    // A gift for zero number of node.
    end_pfn = ustl::mm::align_up(end_pfn, MemMap::FRAMES_PER_MEMMAP_CHUNK);
    if (hole_start_pfn < end_pfn) {
        init_memmap_reserved(hole_start_pfn, end_pfn, ZoneType::Normal, 0);
    }
}

} // namespace ours::mm
