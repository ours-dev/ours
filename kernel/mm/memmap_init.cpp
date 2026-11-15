/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/memmap_init.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>

#include <algorithm>

#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
namespace {
    /// The frames in a hole. They get a descriptor too -- marked reserved, so
    /// nothing hands them out -- because a pfn inside a zone's span has to
    /// resolve to something.
    ///
    /// Unlike the range below, a hole may genuinely have no descriptor behind
    /// it, so this asks pfn_valid() first instead of asserting.
    INIT_CODE
    auto init_memmap_reserved(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NodeId nid) -> void {
        for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
            if (!pfn_valid(pfn)) {
                continue;
            }

            auto frame = pfn_to_frame(pfn);
            frame->init(ztype, pfn_to_secnum(pfn), nid);
            frame->mark_reserved();
        }
    }

    INIT_CODE
    auto init_memmap_range(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NodeId nid) -> void {
        for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
            auto frame = pfn_to_frame(pfn);
            DEBUG_ASSERT(frame, "A usable pfn with no descriptor behind it");

            frame->init(ztype, pfn_to_secnum(pfn), nid);
        }
    }

    /// Descriptors for the overlap between [start, end) and the part of `zone`
    /// that is really there. Returns the first pfn it did not initialise.
    INIT_CODE
    auto init_memmap_zone(ZoneType zone, Pfn zone_start, Pfn zone_end, Pfn start, Pfn end,
                          Pfn hole_start, NodeId nid) -> Pfn {
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

    /// What a frame inside a reservation of |type| is.
    ///
    /// Only PageTable has an owner in the mm vocabulary: the aspace that built
    /// it walks it, so its frames take Role::Mmu. The rest are memory the
    /// kernel or the boot chain simply holds -- an image, a blob, a descriptor
    /// array -- with no mm-side owner to name, so they keep Role::None and are
    /// described by the Reserved flag alone.
    [[nodiscard]]
    INIT_CODE
    auto role_of(bootmem::MemoryType const type) -> page::Role {
        return type == bootmem::MemoryType::PageTable ? page::Role::Mmu : page::Role::None;
    }

    /// Stamps every frame the boot chain claimed with what it holds.
    ///
    /// Until the reservations carried a type this was impossible: a reserved
    /// range was a bare address range, so a page table the CPU is walking, the
    /// kernel image and a hole the firmware declared were one fact, and the only
    /// thing the memmap could say about any of them was nothing at all -- frames
    /// inside a reservation came out of init_memmap_range() with Role::None and
    /// not even the Reserved flag, which init_memmap_reserved() gave only to
    /// holes. The single record that such a frame was taken was that the buddy
    /// never received it.
    ///
    /// This also gives PmZone::m_reserved_frames its first writer. The field has
    /// always been documented as `present_frames - managed_frames` and printed
    /// by PmNode::dump(), and has always read zero.
    ///
    /// Runs after the descriptors exist and before EarlyMem::do_handoff(), which
    /// is the only window where both halves are true: the frames have
    /// descriptors to stamp, and every reservation that will ever matter is
    /// already on the books -- the phys chain's, replayed by
    /// handoff_early_pmm(), and the kernel's own (node and zone descriptors, the
    /// memmap itself, the boot CPU's provisional per-CPU unit, the AP
    /// trampoline). The one type that appears later is Mmu, which do_handoff()
    /// creates for the ranges it gives the buddy; those frames are the buddy's
    /// and get their role from it.
    INIT_CODE
    auto stamp_reserved_frames() -> void {
        auto frames_by_type = std::array<usize, usize(bootmem::MemoryType::Count)>{};

        EarlyMem::visit_reservations([&](bootmem::Reservation const reservation) {
            // Transient is deliberately left alone. Those frames go back to the
            // buddy once their one user is done (CpuLocal::init() releases the
            // provisional unit, free_bootstrap16_buffer() the trampoline), so
            // counting them as reserved would make the tally wrong from the
            // moment they return -- and nothing clears the flag on the way out.
            if (reservation.type == bootmem::MemoryType::Transient) {
                return true;
            }

            auto const role = role_of(reservation.type);
            auto const start_pfn = phys_to_pfn(reservation.range.base);
            auto const end_pfn = phys_to_pfn(ustl::round_up(reservation.range.end(), PAGE_SIZE));

            for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
                if (!pfn_valid(pfn)) {
                    // A reservation outside usable RAM -- a peripheral window, a
                    // firmware hole -- has no descriptor to stamp.
                    continue;
                }
                auto *const frame = pfn_to_frame(pfn);
                if (!(frame->flags() & page::Flags::Initialized)) {
                    // Inside a section, outside every zone: the walk above never
                    // reached it, so there is nothing consistent to add to.
                    continue;
                }
                if (!!(frame->flags() & page::Flags::Reserved)) {
                    // Already on the books, or never to be on them: a frame
                    // shared with an earlier reservation (two reservations
                    // meeting inside one page), or a hole, which
                    // init_memmap_reserved() marked and which is not a present
                    // frame. Counting either would make the reserved count
                    // exceed what is there.
                    //
                    // This used to happen by accident: mark_reserved()
                    // assigned the whole flag field, erasing Initialized, so
                    // the test above skipped both. Neither case occurs on the
                    // QEMU images today (none of their reservations shares a
                    // frame or covers a hole), but a firmware map is not ours
                    // to promise.
                    continue;
                }

                frame->mark_reserved();
                if (role != page::Role::None) {
                    frame->set_role(role);
                }

                auto *const node = find_node(frame->nid());
                if (node == nullptr) {
                    continue;
                }
                if (auto *const zone = PmZone::at(frame->nid(), frame->zone())) {
                    zone->add_reserved_frames(1);
                }
                frames_by_type[usize(reservation.type)] += 1;
            }
            return true;
        });

        // The line that makes the stamping checkable, next to
        // free_unused_memory()'s "Released N regions" and there for the same
        // reason: a total nobody can read is a total nobody can be wrong about.
        // One line per type that has frames, so the list says which kinds of
        // early memory this boot actually produced rather than a single number
        // that hides them.
        for (auto index = usize{0}; index < frames_by_type.size(); ++index) {
            if (frames_by_type[index] == 0) {
                continue;
            }
            log::info("Reserved {} frames ({} KiB) of {}", frames_by_type[index],
                      (frames_by_type[index] * PAGE_SIZE) >> 10,
                      bootmem::to_string(bootmem::MemoryType(index)));
        }
    }
} // namespace

INIT_CODE
auto build_memmap() -> Status {
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
    // built, so the descriptors have to be there first.
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

INIT_CODE
auto init_memmap() -> void {
    Pfn hole_start_pfn = 0, start_pfn = 0, end_pfn = 0;
    [[maybe_unused]]
    auto const status = EarlyMem::visit_usable(
        EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
        [&](bootmem::MemoryRegion const region) {
            auto const node = find_node(region.node);
            DEBUG_ASSERT(node != nullptr, "");

            start_pfn = phys_to_pfn(region.range.base);
            end_pfn = phys_to_pfn(region.range.end());
            node->zone_list()->for_each_local_zone([&](PmZone &zone) {
                start_pfn = init_memmap_zone(zone.zone_type(), zone.start_pfn(), zone.end_pfn(),
                                             start_pfn, end_pfn, hole_start_pfn, region.node);

                // The cursor, not the region end. The hole mark says how far
                // descriptors actually reach, and a zone that does not reach
                // what is left of this region hands the cursor back unmoved --
                // so it must not move either.
                //
                // Pushing it to end_pfn regardless was wrong in exactly one
                // case, and that case is the common one: zones are walked in
                // type order, so for a region lying entirely above Dma, the Dma
                // pass initialises nothing yet still advanced the mark to the
                // end of the region. The Dma32 pass that really covers the
                // region then saw `hole_start >= first` and concluded there was
                // no hole in front of it, leaving the gap between this region
                // and the previous one with no reserved descriptors at all --
                // frames left all-zero, without even Flags::Initialized, which
                // is what hands the buddy a descriptor it should never see.
                hole_start_pfn = start_pfn;
            });
            return true;
        });
    DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate early memory");

    // A gift for zero number of node.
    end_pfn = ustl::round_up(end_pfn, MemMap::FRAMES_PER_MEMMAP_CHUNK);
    if (hole_start_pfn < end_pfn) {
        init_memmap_reserved(hole_start_pfn, end_pfn, ZoneType::Normal, 0);
    }

    // Last, because it reads the descriptors the walk above created.
    stamp_reserved_frames();
}

} // namespace ours::mm
