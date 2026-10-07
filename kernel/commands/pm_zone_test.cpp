/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// `pmzone.test`: a max-order block freed back into its zone one frame at a
/// time comes back together as one max-order block, whichever memmap section
/// of the zone it lies in.
///
/// Why sections: the sparse memmap gives each section its own descriptor
/// array, allocated apart from the others, so a zone that spans several
/// sections has its descriptors in several arrays. The buddy finds a freed
/// block's partner from the descriptor in hand, never more than one max-order
/// block away, which a section always holds whole. It used to index every
/// descriptor of the zone off the zone's first one, as though they were one
/// array: in every section but the first the partner was never found, and a
/// block split there stayed split for good. Nothing at boot looks at
/// coalescing, so this test does.
///
/// For each zone spanning more than one section it takes a max-order block
/// from the zone's first section, the control, and one from a later section,
/// frees each back frame by frame and checks that the zone gained exactly one
/// free max-order block and nothing at any lower order. To reach a later
/// section it may have to take every max-order block of the first; those go
/// back untouched at the end.
///
/// The counts are read under the zone lock, but not all at once: another CPU
/// allocating from or freeing into the zone between the two readings would
/// move them. Run it from an idle shell.
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>

#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using mm::PmFrame;
    using mm::PmNode;
    using mm::PmZone;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pmzone.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pmzone.test: FAIL  {}", name);
    }

    /// Frames in a max-order block.
    constexpr auto BLOCK_FRAMES = usize{BIT(MAX_FRAME_ORDER)};

    /// Max-order blocks in one section: the most a zone's first section can
    /// hand out before an allocation has to come from a later one.
    constexpr auto BLOCKS_PER_SECTION = usize{BIT(PFN_SECTION_SHIFT - MAX_FRAME_ORDER)};

    using Counts = std::array<usize, NR_FRAME_ORDERS>;

    auto count_free_blocks(PmZone &zone) -> Counts {
        Counts counts{};
        for (usize order = 0; order < NR_FRAME_ORDERS; ++order) {
            counts[order] = zone.free_blocks(order);
        }
        return counts;
    }

    /// Frees |block|, a max-order block taken from |zone|, one frame at a time
    /// and says whether the zone's lists gained one max-order block and
    /// nothing below it. The lower half goes top down, so each frame freed
    /// finds its partner above it; the upper half bottom up, so each finds it
    /// below: the merge steps both ways from the descriptor in hand.
    auto frees_back_whole(PmZone &zone, PmFrame *const block, char const *const where) -> bool {
        auto const pfn = mm::frame_to_pfn(block);
        auto const before = count_free_blocks(zone);

        constexpr auto HALF = BLOCK_FRAMES / 2;
        for (auto i = HALF; i > 0; --i) {
            zone.free_frame(block + (i - 1), 0);
        }
        for (auto i = HALF; i < BLOCK_FRAMES; ++i) {
            zone.free_frame(block + i, 0);
        }

        auto const after = count_free_blocks(zone);
        auto whole = true;
        for (usize order = 0; order < NR_FRAME_ORDERS; ++order) {
            auto expected = before[order];
            if (order == MAX_FRAME_ORDER) {
                expected += 1;
            }
            if (after[order] != expected) {
                whole = false;
                log::info("pmzone.test:     order {}: {} free block(s) before, {} after", order,
                          before[order], after[order]);
            }
        }
        auto outcome = "coalesced";
        if (!whole) {
            outcome = "did not coalesce";
        }
        log::info("pmzone.test:   {} zone, {}: block at pfn {:#x} (section {}) {}",
                  mm::to_string(zone.zone_type()), where, pfn, mm::pfn_to_secnum(pfn), outcome);
        return whole;
    }

    /// Whether |zone| spans more than one section, and so has descriptors in
    /// more than one array.
    auto spans_sections(PmZone const &zone) -> bool {
        return mm::pfn_to_secnum(zone.start_pfn()) != mm::pfn_to_secnum(zone.end_pfn() - 1);
    }

    /// The checks for one zone spanning several sections. Returns whether a
    /// block from a later section was found to check.
    auto check_zone(Tally &tally, PmZone &zone) -> bool {
        auto const first_section = mm::pfn_to_secnum(zone.start_pfn());

        PmFrame *first = nullptr;
        PmFrame *later = nullptr;
        PmFrame::List spare;
        for (usize taken = 0; taken <= BLOCKS_PER_SECTION && !later; ++taken) {
            auto *const block = zone.alloc_frame(mm::GAF_KERNEL, MAX_FRAME_ORDER);
            if (!block) {
                break;
            }
            if (mm::pfn_to_secnum(mm::frame_to_pfn(block)) != first_section) {
                later = block;
            } else if (!first) {
                first = block;
            } else {
                spare.push_back(*block);
            }
        }

        if (first) {
            verdict(tally, frees_back_whole(zone, first, "first section"),
                    "a block freed frame by frame in the zone's first section coalesces whole");
        }
        if (later) {
            verdict(tally, frees_back_whole(zone, later, "later section"),
                    "a block freed frame by frame in a later section coalesces whole");
        } else {
            log::info("pmzone.test:   {} zone: no free max-order block outside section {}",
                      mm::to_string(zone.zone_type()), first_section);
        }

        spare.clear_and_dispose(
            [&zone](PmFrame *frame) noexcept { zone.free_frame(frame, MAX_FRAME_ORDER); });
        return later != nullptr;
    }

    auto pmzone_test_command(int, char **) -> int {
        Tally tally;
        usize spanning = 0;
        usize checked = 0;

        PmNode::for_each_node(PmNode::State::Online, [&](PmNode &node) {
            node.zone_list()->for_each_local_zone([&](PmZone &zone) {
                if (!spans_sections(zone)) {
                    return;
                }
                spanning += 1;
                if (check_zone(tally, zone)) {
                    checked += 1;
                }
            });
        });

        if (spanning == 0) {
            log::info("pmzone.test:   no zone spans two sections; nothing to check");
        } else {
            verdict(tally, checked != 0,
                    "some zone spanning two sections had a free max-order block past its first");
        }

        if (tally.failed != 0) {
            log::error("pmzone.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pmzone.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PmZoneTest, "pmzone.test", ours::pmzone_test_command,
             "check that a zone's free blocks coalesce in every memmap section it spans");
