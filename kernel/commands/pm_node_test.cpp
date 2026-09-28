/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// `pmnode.test`: checks the physical-memory node layer on the machine it runs
/// on -- whatever node layout that is, one node or several.
///
/// What it covers, and why each part is here:
///
///   - Topology. The CPU-to-node tables and the distance matrix moved into
///     PmNode::Topology; the calling CPU must resolve to a real node, the same
///     one mm::current_node() gives, and no CPU may have been moved off its
///     firmware node at boot.
///   - ZoneList. The by-type array and the fallback order are the only two
///     lists a node keeps now. Each node's own zones must be the head of its
///     fallback order, in type order; no zone may appear twice; the remote
///     zones must come nearest node first.
///   - Router. The fallback order is built from it, so it is checked on its
///     own: every other node with memory exactly once, never the node itself,
///     bounded by the candidate mask, and for_each_node() visiting the same.
///   - ZoneIterator and AllocationContext. Rewritten with ZoneList and not on
///     the allocation path yet, so nothing else exercises them: the three
///     filters (holes, node mask, zone type) and the ThisNode choice.
///   - contains(). Declared for a long time and defined only recently.
///   - Fragment and bulk allocation. alloc_frames_fragment() used to loop for
///     ever on an even page count.
///   - MemoryAllocationState. A thread that disabled allocation must be
///     refused on the node-less path and on the path that names a node alike.
///
/// A kshell command rather than a host test for the same reason usercopy.test
/// is one: PmNode is built on EarlyMem and the buddy, which exist only in a
/// booted kernel.
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/node_mask.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/zone_iterator.hpp>
#include <ours/task/thread.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/bit.hpp>

namespace ours {
namespace {
    using mm::NodeMask;
    using mm::NR_ZONES_PER_NODE;
    using mm::PmFrame;
    using mm::PmNode;
    using mm::PmZone;
    using mm::ZoneList;
    using mm::ZoneType;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pmnode.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pmnode.test: FAIL  {}", name);
    }

    auto run_topology_checks(Tally &tally) -> void {
        auto const cpu = mm::CpuLocal::current_cpu();
        auto const nid = PmNode::Topology::current_node();
        log::info("pmnode.test:   cpu{} is on node {}; {} node(s) online", cpu, nid,
                  PmNode::num_online_nodes());

        verdict(tally, nid < NUMA_MAX_NODE && PmNode::node(nid) != nullptr,
                "the calling CPU resolves to a node that exists");
        verdict(tally, nid == PmNode::Topology::cpu_to_node(cpu) && nid == mm::current_node(),
                "current_node() agrees with cpu_to_node() and mm::current_node()");

        auto unmoved = true;
        Cpu::possible_mask().for_each([&unmoved](CpuNum const each) {
            if (PmNode::Topology::cpu_to_node(each) != PmNode::Topology::firmware_node(each)) {
                unmoved = false;
            }
        });
        verdict(tally, unmoved, "every possible CPU is still on its firmware node");

        auto const online = PmNode::num_online_nodes();
        verdict(tally, online >= 1 && online == PmNode::node_online_mask().count(),
                "num_online_nodes() counts the Online mask");
    }

    /// The checks one node's ZoneList must pass; see the file comment.
    auto check_zone_list(Tally &tally, PmNode &node) -> void {
        auto const nid = node.nid();
        auto const *const list = node.zone_list();

        // By type: every non-null slot holds a zone of that type on this node,
        // and for_each_local_zone() visits exactly those, in type order.
        auto by_type_ok = true;
        usize local_count = 0;
        for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
            auto const type = ZoneType(i);
            if (auto *const zone = list->local_zone(type)) {
                local_count += 1;
                if (zone->which_node() != nid || zone->zone_type() != type) {
                    by_type_ok = false;
                }
            }
        }
        usize visited = 0;
        auto last_type = usize(0);
        list->for_each_local_zone([&](PmZone &zone) {
            auto const type = static_cast<usize>(zone.zone_type());
            if (visited != 0 && type <= last_type) {
                by_type_ok = false;
            }
            if (list->local_zone(zone.zone_type()) != &zone) {
                by_type_ok = false;
            }
            last_type = type;
            visited += 1;
        });
        verdict(tally, by_type_ok && visited == local_count && local_count >= 1,
                "a node's local zones are its own, one per type, walked in type order");

        // Fallback order: own zones first, in the same order.
        auto const fallback = list->fallback();
        auto head_ok = fallback.size() >= local_count;
        usize index = 0;
        list->for_each_local_zone([&](PmZone &zone) {
            if (index >= fallback.size() || fallback[index] != &zone) {
                head_ok = false;
            }
            index += 1;
        });
        verdict(tally, head_ok, "a node's fallback order starts with its own zones");

        // No zone twice, every zone on an online node.
        auto unique_ok = true;
        for (usize i = 0; i < fallback.size(); ++i) {
            if (!PmNode::node_online_mask().test(fallback[i]->which_node())) {
                unique_ok = false;
            }
            for (usize j = i + 1; j < fallback.size(); ++j) {
                if (fallback[i] == fallback[j]) {
                    unique_ok = false;
                }
            }
        }
        verdict(tally, unique_ok, "no zone appears twice in a fallback order");

        // Remote zones nearest node first.
        auto distance_ok = true;
        auto last_distance = usize(0);
        for (usize i = local_count; i < fallback.size(); ++i) {
            auto const other = fallback[i]->which_node();
            auto const distance = PmNode::Topology::distance(nid, other);
            if (other == nid || distance < last_distance) {
                distance_ok = false;
            }
            last_distance = distance;
        }
        verdict(tally, distance_ok, "remote zones follow, nearest node first");
    }

    auto check_router(Tally &tally, NnId const nid) -> void {
        auto const &online = PmNode::node_online_mask();
        auto const route = PmNode::Router::build(nid, online);

        NodeMask seen;
        auto route_ok = true;
        auto last_distance = usize(0);
        usize hops = 0;
        for (auto const next: route) {
            auto const distance = PmNode::Topology::distance(nid, next);
            if (next == nid || seen.test(next) || distance < last_distance) {
                route_ok = false;
            }
            seen.set(next);
            last_distance = distance;
            hops += 1;
        }
        usize expected = 0;
        PmNode::for_each_nid(PmNode::State::Memory, [&](NnId const other) {
            if (other != nid && online.test(other)) {
                expected += 1;
                if (!seen.test(other)) {
                    route_ok = false;
                }
            }
        });
        verdict(tally, route_ok && hops == expected,
                "Router::build() lists every other node with memory once, nearest first");

        // One object: begin() and end() of two separate temporaries were being
        // compared here, which are never equal except by the accident of the
        // two landing at the same stack address -- true on one machine, false
        // on the next.
        auto const empty = PmNode::Router::build(nid, NodeMask{});
        verdict(tally, empty.begin() == empty.end(), "Router::build() with no candidates is empty");

        // for_each_node() visits the same nodes, and forwards its arguments.
        usize visits = 0;
        auto visits_ok = true;
        route.for_each_node(
            [&](PmNode &node, usize const tag) {
                if (tag != 7 || !seen.test(node.nid())) {
                    visits_ok = false;
                }
                visits += 1;
            },
            usize(7));
        verdict(tally, visits_ok && visits == hops,
                "Router::for_each_node() visits the route and passes its arguments on");
    }

    auto count_walk(mm::ZoneIterator &walk) -> usize {
        usize count = 0;
        while (walk.move_next()) {
            count += 1;
        }
        return count;
    }

    auto check_zone_iterator(Tally &tally, PmNode &node) -> void {
        auto const *const list = node.zone_list();
        auto const &online = PmNode::node_online_mask();

        usize local_count = 0;
        list->for_each_local_zone([&local_count](PmZone &) { local_count += 1; });

        // Holes: the by-type array has a null for each missing type, and the
        // walk must step over them rather than stop or dereference one.
        mm::ZoneIterator walk;
        walk.set_nodes(online).set_zone_range(ZoneType::Dma, ZoneType::Normal);
        walk.set_zones(list->local_zones());
        verdict(tally, count_walk(walk) == local_count,
                "ZoneIterator steps over the holes in the by-type array");

        // Node filter: excluding this node leaves only the remote zones.
        auto others = online;
        others.set(node.nid(), false);
        walk.set_nodes(others).set_zones(list->fallback());
        auto remote_ok = true;
        usize remote = 0;
        while (auto *const zone = walk.move_next()) {
            if (zone->which_node() == node.nid()) {
                remote_ok = false;
            }
            remote += 1;
        }
        verdict(tally, remote_ok && remote == list->fallback().size() - local_count,
                "ZoneIterator's node mask filters out the excluded node");

        // Type filter: an upper bound of Dma yields only Dma zones.
        walk.set_nodes(online).set_zone_range(ZoneType::Dma, ZoneType::Dma);
        walk.set_zones(list->fallback());
        auto type_ok = true;
        usize dma = 0;
        while (auto *const zone = walk.move_next()) {
            if (zone->zone_type() != ZoneType::Dma) {
                type_ok = false;
            }
            dma += 1;
        }
        usize expected_dma = 0;
        for (auto *const zone: list->fallback()) {
            if (zone->zone_type() == ZoneType::Dma) {
                expected_dma += 1;
            }
        }
        verdict(tally, type_ok && dma == expected_dma,
                "ZoneIterator's type bound filters out higher zone types");

        // AllocationContext: ThisNode walks only the local zones.
        mm::AllocationContext local;
        static_cast<void>(local.build(&node, mm::GAF_KERNEL | mm::Gaf::ThisNode, 0, online));
        auto this_node_ok = true;
        while (auto *const zone = local.ziter.move_next()) {
            if (zone->which_node() != node.nid()) {
                this_node_ok = false;
            }
        }
        // Without ThisNode. GAF_KERNEL itself carries ThisNode (gaf.hpp), so it
        // has to be taken out to ask for the whole fallback order; GAF_KERNEL's
        // zone type is Normal, which lets every zone through.
        mm::AllocationContext everywhere;
        static_cast<void>(everywhere.build(&node, mm::GAF_KERNEL & ~mm::Gaf::ThisNode, 0, online));
        auto const walked = count_walk(everywhere.ziter);
        log::info("pmnode.test:   AllocationContext without ThisNode walked {} of {} zone(s)",
                  walked, list->fallback().size());
        verdict(tally, this_node_ok && walked == list->fallback().size(),
                "AllocationContext walks the local zones for ThisNode, the fallback otherwise");
    }

    auto check_contains(Tally &tally, PmNode &node) -> void {
        auto ok = true;
        for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
            auto const type = ZoneType(i);
            auto *const zone = node.zone_list()->local_zone(type);
            if (!zone) {
                if (node.contains(0, type)) {
                    ok = false; // No zone of this type, so nothing to hold.
                }
                continue;
            }
            if (node.contains(0, type) != zone->contains(0)) {
                ok = false;
            }
            if (node.contains(MAX_FRAME_ORDER + 1, type)) {
                ok = false; // No such order.
            }
        }
        verdict(tally, ok,
                "contains() follows the zone, says no without one, and no past the top order");
    }

    auto check_fragment_and_bulk(Tally &tally) -> void {
        // Even counts first: they are the ones that used to hang.
        auto fragment_ok = true;
        for (usize const n: {usize(2), usize(6), usize(8), usize(5), usize(1)}) {
            auto result = mm::alloc_frames_fragment(mm::GAF_KERNEL, n);
            if (!result) {
                fragment_ok = false;
                continue;
            }
            usize blocks = 0;
            usize pages = 0;
            for (auto &frame: *result) {
                blocks += 1;
                pages += BIT(frame.order());
            }
            log::info("pmnode.test:   alloc_frames_fragment({}) -> {} block(s), {} page(s)", n,
                      blocks, pages);
            if (pages != n || blocks != static_cast<usize>(ustl::popcount(n))) {
                fragment_ok = false;
            }
            mm::free_frames(&*result);
        }
        verdict(tally, fragment_ok,
                "alloc_frames_fragment() returns one block per set bit, n pages in all");

        PmFrame::List list;
        auto bulk_ok =
            Status::Ok == mm::alloc_frames(NnId(NUMA_MAX_NODE), mm::GAF_KERNEL, &list, 4);
        usize frames = 0;
        PmFrame const *previous = nullptr;
        for (auto &frame: list) {
            if (frame.order() != 0 || &frame == previous) {
                bulk_ok = false;
            }
            previous = &frame;
            frames += 1;
        }
        mm::free_frames(&list);
        verdict(tally, bulk_ok && frames == 4, "alloc_frames_bulk() returns n single frames");
    }

    auto check_disabled_allocation(Tally &tally) -> void {
        auto &state = task::Thread::Current::memory_allocation_state();

        state.disable();
        auto *const nodeless = mm::alloc_frames(mm::GAF_KERNEL, usize(0));
        auto *const by_node = mm::alloc_frames(mm::current_node(), mm::GAF_KERNEL, usize(0));
        state.enable();

        auto *const after = mm::alloc_frames(mm::current_node(), mm::GAF_KERNEL, usize(0));
        verdict(tally, !nodeless && !by_node,
                "a thread that disabled allocation is refused with or without a node");
        verdict(tally, after != nullptr, "enabling it again lets the allocation through");

        // Freed even if the check above failed, so the test leaks nothing.
        for (auto *const frame: {nodeless, by_node, after}) {
            if (frame) {
                mm::free_frames(frame, 0);
            }
        }
    }

    auto pmnode_test_command(int, char **) -> int {
        Tally tally;

        run_topology_checks(tally);
        PmNode::for_each_node(PmNode::State::Online, [&tally](PmNode &node) {
            log::info("pmnode.test: node {}", node.nid());
            check_zone_list(tally, node);
            check_router(tally, node.nid());
            check_zone_iterator(tally, node);
            check_contains(tally, node);
        });
        check_fragment_and_bulk(tally);
        check_disabled_allocation(tally);

        if (tally.failed != 0) {
            log::error("pmnode.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pmnode.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PmNodeTest, "pmnode.test", ours::pmnode_test_command,
             "check the physical-memory node layer: topology, zone lists, allocation");
