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
///   - The fallback order derivation. It is numa::build_fallback_orders() now,
///     and its properties are checked on the host where a matrix can be made
///     up (//kernel/lib/numa/test:fallback_order); what is checked here is the
///     derivation against this machine's own masks -- every node with memory
///     exactly once, never the node itself, nearest first.
///   - ZoneSelector, which every allocation now walks: its three filters
///     (holes in the by-type array, the request's node mask, the request's
///     zone-type range in both directions) and AllocFlags::HomeIfAllowed, which is served
///     by resolve_request() narrowing the mask rather than by switching lists.
///     These cases were written against ZoneIterator and AllocationContext,
///     which were on no allocation path and are gone; they test the real one
///     now, almost unchanged.
///   - contains(). Declared for a long time and defined only recently.
///   - Fragment and bulk allocation. alloc_frames_fragment() used to loop for
///     ever on an even page count. Use --exhaust-remote from an idle guest
///     with a memory-only NUMA node to check fallback and partial rollback.
///   - Static allocation. Direct class calls honor node restrictions, errors,
///     and allocation-disable state, and release without a node instance.
///   - Count helpers. Both direct-map overloads round three pages up to a
///     usable, zeroed four-page block, including the node-specific template.
///   - AllocState. A thread that disabled allocation must be
///     refused on the node-less path and on the path that names a node alike.
///
/// A kshell command rather than a host test for the same reason usercopy.test
/// is one: PmNode is built on EarlyMem and the buddy, which exist only in a
/// booted kernel.
#include <ours/cpu.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/node_placement.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/zone_selector.hpp>
#include <ours/node_mask.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>
#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <numa/fallback_order.hpp>
#include <numa/topology.hpp>
#include <ustl/bit.hpp>

namespace ours {
namespace {
    using mm::NR_ZONES_PER_NODE;
    using mm::PmFrame;
    using mm::PmNode;
    using mm::PmZone;
    using mm::ZoneList;
    using mm::ZoneType;
    using numa::Topology;
    using ours::NodeMask;

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
        auto const nid = mm::current_node();
        log::info("pmnode.test:   cpu{} is on node {}; {} node(s) online", cpu, nid,
                  PmNode::num_online_nodes());

        verdict(tally, nid < MAX_NODES && PmNode::node(nid) != nullptr,
                "the calling CPU resolves to a node that exists");
        // There is one current_node() now. It used to be three -- a
        // NumaTopology::current_node(), mm::current_node() wrapping it, and
        // the table read -- and this case compared all three. With the
        // topology's own gone, what is left to check is that the wrapper
        // really reads the placement table for the calling CPU rather than
        // answering from somewhere else.
        verdict(tally, nid == mm::NodePlacement::node_of(cpu),
                "current_node() is the placement table's answer for the calling CPU");

        // Only the CPUs the firmware actually placed. The two tables mean
        // different things now that the running placement is the node layer's
        // own: a CPU the firmware said nothing about reads MAX_NODES there and
        // sits on the bring-up fallback here, and that is not a move. On this
        // build nothing writes the firmware table at all -- srat.cpp compiles
        // only under mm_numa -- so this check passes by having nothing to
        // check, and starts meaning something on a NUMA configuration.
        //
        // Except where the firmware placed a CPU on a node that does not
        // exist: a proximity domain with CPUs and no memory is not brought up
        // as a node, so its CPUs have to run from one that is. Those are
        // counted apart, and checked for the thing that matters about them --
        // that wherever they ended up, there is a node there.
        auto unmoved = true;
        auto rehomed = true;
        usize declared_cpus = 0;
        usize memoryless_cpus = 0;
        Cpu::possible_mask().for_each(
            [&unmoved, &rehomed, &declared_cpus, &memoryless_cpus](CpuNum const each) {
                auto const declared = Topology::firmware_node(each);
                if (declared >= MAX_NODES) {
                    return;
                }
                declared_cpus += 1;
                auto const placed = mm::NodePlacement::node_of(each);
                if (PmNode::node(declared) == nullptr) {
                    memoryless_cpus += 1;
                    if (placed >= MAX_NODES || PmNode::node(placed) == nullptr) {
                        rehomed = false;
                    }
                    return;
                }
                if (placed != declared) {
                    unmoved = false;
                }
            });
        log::info("pmnode.test:   {} of {} possible CPU(s) were placed by firmware", declared_cpus,
                  Cpu::possible_mask().count());
        if (memoryless_cpus != 0) {
            log::info("pmnode.test:   {} of them on a node with no memory", memoryless_cpus);
        }
        verdict(tally, unmoved,
                "no CPU the firmware placed on a node that exists has been moved off it");
        verdict(tally, rehomed,
                "every CPU the firmware placed on a node with no memory runs from one that exists");

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
            auto const distance = Topology::distance(nid, other);
            if (other == nid || distance < last_distance) {
                distance_ok = false;
            }
            last_distance = distance;
        }
        verdict(tally, distance_ok, "remote zones follow, nearest node first");
    }

    auto check_fallback_orders(Tally &tally, NodeId const nid) -> void {
        auto const &online = PmNode::node_online_mask();
        auto const members = online & PmNode::node_memory_mask();

        // Static: MAX_NODES rows is kilobytes on a NUMA build, and a kshell
        // command runs on an ordinary thread stack. Single-threaded, like the
        // rest of this test.
        static numa::FallbackOrders orders;
        numa::build_fallback_orders(online, members, &orders);

        NodeMask seen;
        auto route_ok = true;
        auto last_distance = usize(0);
        usize hops = 0;
        for (auto const next: orders[nid]) {
            auto const distance = Topology::distance(nid, next);
            if (next == nid || seen.test(next) || distance < last_distance) {
                route_ok = false;
            }
            seen.set(next);
            last_distance = distance;
            hops += 1;
        }
        usize expected = 0;
        members.for_each([&](NodeId const other) {
            if (other != nid) {
                expected += 1;
                if (!seen.test(other)) {
                    route_ok = false;
                }
            }
        });
        verdict(tally, route_ok && hops == expected,
                "build_fallback_orders() lists every other node with memory once, nearest first");

        // No members, so nothing can be a hop -- but every node in the first
        // set still gets a row, and here every row must be empty.
        //
        // The begin()/end() comparison this replaced was made on two separate
        // temporaries, which are never equal except by the accident of the two
        // landing at the same stack address -- true on one machine, false on
        // the next. One table, so the row compared is the row filled.
        numa::build_fallback_orders(online, NodeMask{}, &orders);
        verdict(tally, orders[nid].begin() == orders[nid].end(),
                "build_fallback_orders() with no members leaves an empty order");
    }

    auto count_selected(mm::ZoneSelector const &selector) -> usize {
        usize count = 0;
        for (auto it = selector.begin(); it != selector.end(); ++it) {
            count += 1;
        }
        return count;
    }

    /// A request over every online node, which each case then narrows.
    auto request_for(PmNode &node, mm::AllocFlags const alloc_flags) -> mm::AllocRequest {
        return mm::resolve_request(alloc_flags, 0, PmNode::node_online_mask(),
                                   mm::Origin{.node = node.nid(), .cpu = Cpu::id()});
    }

    auto check_zone_selector(Tally &tally, PmNode &node) -> void {
        auto const *const list = node.zone_list();

        usize local_count = 0;
        list->for_each_local_zone([&local_count](PmZone &) { local_count += 1; });

        usize dma_count = 0;
        for (auto *const zone: list->fallback()) {
            if (zone->zone_type() == ZoneType::Dma) {
                dma_count += 1;
            }
        }

        // Holes: the by-type array has a null for each missing type, and the
        // selector must step over them rather than stop or dereference one.
        auto const all = request_for(node, mm::ALLOC_KERNEL);
        mm::ZoneSelector const holes{list->local_zones(), all};
        verdict(tally, count_selected(holes) == local_count,
                "ZoneSelector steps over the holes in the by-type array");

        // Node filter: excluding this node leaves only the remote zones.
        auto without_local = all.allowed_nodes();
        without_local.set(node.nid(), false);
        auto const others = all.with_nodes(without_local);
        mm::ZoneSelector const remote_walk{list->fallback(), others};
        auto remote_ok = true;
        usize remote = 0;
        for (auto &zone: remote_walk) {
            if (zone.which_node() == node.nid()) {
                remote_ok = false;
            }
            remote += 1;
        }
        verdict(tally, remote_ok && remote == list->fallback().size() - local_count,
                "ZoneSelector's node mask filters out the excluded node");

        // Upper bound: Dma as the ceiling yields only Dma zones.
        auto const low_only = all.with_zone_range(all.lowest_zone(), ZoneType::Dma);
        mm::ZoneSelector const low_walk{list->fallback(), low_only};
        auto upper_ok = true;
        usize dma = 0;
        for (auto &zone: low_walk) {
            if (zone.zone_type() != ZoneType::Dma) {
                upper_ok = false;
            }
            dma += 1;
        }
        verdict(tally, upper_ok && dma == dma_count,
                "ZoneSelector's upper bound filters out higher zone types");

        // Lower bound, which the walk this replaced stored and never read: a
        // floor of Dma32 must keep a caller off the Dma zones entirely. This
        // is the case that could not have been written before.
        auto const high_only = all.with_zone_range(ZoneType::Dma32, all.highest_zone());
        mm::ZoneSelector const high_walk{list->fallback(), high_only};
        auto lower_ok = true;
        usize above = 0;
        for (auto &zone: high_walk) {
            if (zone.zone_type() < ZoneType::Dma32) {
                lower_ok = false;
            }
            above += 1;
        }
        verdict(tally, lower_ok && above == list->fallback().size() - dma_count,
                "ZoneSelector's lower bound keeps a caller off the zones below it");

        // AllocFlags::HomeIfAllowed, through the path that serves it: resolve_request()
        // narrows the request's node mask, so the selector over the whole
        // fallback order yields this node's zones and no others. The walk this
        // replaced switched between two candidate lists instead, which is a
        // second mechanism for the same intent.
        auto const home_if_allowed =
            request_for(node, mm::ALLOC_KERNEL | mm::AllocFlags::HomeIfAllowed);
        mm::ZoneSelector const local_walk{list->fallback(), home_if_allowed};
        auto home_if_allowed_ok = true;
        usize local_selected = 0;
        for (auto &zone: local_walk) {
            if (zone.which_node() != node.nid()) {
                home_if_allowed_ok = false;
            }
            local_selected += 1;
        }
        log::info("pmnode.test:   HomeIfAllowed selected {} of {} zone(s) in the fallback order",
                  local_selected, list->fallback().size());
        verdict(tally, home_if_allowed_ok && local_selected == local_count,
                "AllocFlags::HomeIfAllowed narrows the request's node mask, and the selector "
                "honours it");
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
            auto result = mm::alloc_frames_fragment(mm::ALLOC_KERNEL, n);
            if (!result) {
                fragment_ok = false;
                continue;
            }
            usize blocks = 0;
            usize pages = 0;
            for (auto &frame: *result) {
                blocks += 1;
                pages += usize{1} << frame.order();
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
            Status::Ok == mm::alloc_frames(NodeId(MAX_NODES), mm::ALLOC_KERNEL, &list, 4);
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

    auto check_static_allocation(Tally &tally) -> void {
        auto const release = [](ktl::Result<PmFrame *> const &result) {
            if (result && *result) {
                PmNode::free_frames(*result, (*result)->order());
            }
        };
        auto const alloc_flags = mm::ALLOC_KERNEL | mm::AllocFlags::Zero;
        auto const nid = mm::current_node();

        auto implicit = PmNode::alloc_frames(alloc_flags);
        verdict(tally, implicit && *implicit && (*implicit)->order() == 0,
                "static allocation needs no node instance");
        release(implicit);

        auto explicit_home = PmNode::alloc_frames(nid, alloc_flags | mm::AllocFlags::HomeIfAllowed);
        verdict(tally, explicit_home && *explicit_home && (*explicit_home)->nid() == nid,
                "static allocation honors an explicit home and HomeIfAllowed");
        release(explicit_home);

        auto empty = PmNode::alloc_frames(nid, alloc_flags, 0, NodeMask{});
        verdict(tally, !empty && empty.unwrap_err() == Status::OutOfMem,
                "static allocation cannot escape an empty allowed-node set");
        release(empty);

        auto oversized = PmNode::alloc_frames(nid, alloc_flags, MAX_FRAME_ORDER + 1);
        verdict(tally, !oversized && oversized.unwrap_err() == Status::InvArgs,
                "static allocation rejects an order past the maximum");
        release(oversized);

        auto remote_nodes = PmNode::node_memory_mask();
        remote_nodes.set(nid, false);
        if (remote_nodes.any()) {
            auto remote = PmNode::alloc_frames(nid, alloc_flags, 0, remote_nodes);
            verdict(tally, remote && *remote && remote_nodes.test((*remote)->nid()),
                    "static allocation reaches another node permitted by the mask");
            release(remote);
        } else {
            log::info("pmnode.test: skip  cross-node allocation needs multiple memory nodes");
        }
    }

    // The low bit allocates a page before the high bit requests an unsupported block.
    constexpr auto PARTIAL_FRAGMENT_COUNT = usize{1} + (usize{1} << (MAX_FRAME_ORDER + 1));

    auto check_batch_errors(Tally &tally) -> void {
        auto const nid = mm::current_node();
        auto const flags = mm::ALLOC_KERNEL;
        auto empty_fragment = PmNode::alloc_frames_fragment(NodeId(MAX_NODES), flags, 0);
        auto empty_bulk = PmNode::alloc_frames_bulk(flags, 0);
        verdict(tally,
                empty_fragment && empty_fragment->empty() && empty_bulk && empty_bulk->empty(),
                "zero-page batch requests return empty lists");
        if (empty_fragment) {
            PmNode::free_frames(&*empty_fragment);
        }
        if (empty_bulk) {
            PmNode::free_frames(&*empty_bulk);
        }

        auto fragment = mm::alloc_frames_fragment(flags, PARTIAL_FRAGMENT_COUNT, NodeMask::of(nid));
        verdict(tally, !fragment && fragment.unwrap_err() == Status::InvArgs,
                "a fragment request fails when a later block exceeds the maximum order");
        if (fragment) {
            mm::free_frames(&*fragment);
        }

        PmFrame::List list;
        auto const status = mm::alloc_frames(nid, flags, &list, 1, NodeMask{});
        verdict(tally, status == Status::OutOfMem && list.empty(),
                "the bulk interface reports an exhausted candidate set");
        mm::free_frames(&list);
    }

    auto check_node_queries(Tally &tally) -> void {
        auto const nid = mm::current_node();
        auto *const node = PmNode::node(nid);
        verdict(tally,
                node && mm::num_nodes() == PmNode::num_nodes() &&
                    mm::node_work_cpus(nid) == node->work_cpus() &&
                    mm::node_soft_affinity(nid) == &node->soft_affinity() &&
                    mm::node_hard_affinity(nid) == &node->hard_affinity(),
                "PMM topology queries describe the current node");
        for (NodeId absent = 0; absent < MAX_NODES; ++absent) {
            if (PmNode::node(absent)) {
                continue;
            }
            auto single = PmNode::alloc_frames(absent, mm::ALLOC_KERNEL);
            auto bulk = PmNode::alloc_frames_bulk(absent, mm::ALLOC_KERNEL, 1);
            auto fragment = PmNode::alloc_frames_fragment(absent, mm::ALLOC_KERNEL, 1);
            verdict(tally,
                    !mm::node_work_cpus(absent).any() && !mm::node_hard_affinity(absent) &&
                        !mm::node_soft_affinity(absent) && !single &&
                        single.unwrap_err() == Status::InvArgs && !bulk &&
                        bulk.unwrap_err() == Status::InvArgs && !fragment &&
                        fragment.unwrap_err() == Status::InvArgs,
                    "queries and allocation handle an absent node ID");
            if (single) {
                PmNode::free_frames(*single, (*single)->order());
            }
            if (bulk) {
                PmNode::free_frames(&*bulk);
            }
            if (fragment) {
                PmNode::free_frames(&*fragment);
            }
            return;
        }
    }

    auto check_exhausted_node(Tally &tally, NodeId const nid) -> void {
        auto const flags = mm::ALLOC_KERNEL | mm::AllocFlags::HomeIfAllowed;
        auto const mask = NodeMask::of(nid);
        auto const reserved = PmNode::alloc_frames(nid, flags, 0, mask);
        if (!reserved) {
            verdict(tally, false, "reserve a page on the memory-only node");
            return;
        }

        PmFrame::List held;
        for (usize order = MAX_FRAME_ORDER + 1; order > 0; --order) {
            while (auto block = PmNode::alloc_frames(nid, flags, order - 1, mask)) {
                held.push_back(**block);
            }
        }
        auto const fallback = PmNode::alloc_frames(nid, mm::ALLOC_KERNEL);
        auto const fell_back = fallback && (*fallback)->nid() != nid;
        if (fallback) {
            PmNode::free_frames(*fallback, 0);
        }

        auto *const last = *reserved;
        auto *const zone = PmZone::at(nid, last->zone());
        auto const drained = zone && zone->count_free_frames() == 0 && zone->free_frames() == 0 &&
                             !zone->watermark_ok(0, PmZone::WaterMark::Min);
        PmNode::free_frames(last, 0);
        auto bulk = PmNode::alloc_frames_bulk(nid, flags, 2, mask);
        auto const rejected = !bulk && bulk.unwrap_err() == Status::OutOfMem;
        if (bulk) {
            PmNode::free_frames(&*bulk);
        }
        auto const restored = PmNode::alloc_frames(nid, flags, 0, mask);
        auto const restored_last = restored && *restored == last;
        if (restored) {
            PmNode::free_frames(*restored, 0);
        }

        auto fragment = PmNode::alloc_frames_fragment(nid, flags, PARTIAL_FRAGMENT_COUNT, mask);
        auto const invalid = !fragment && fragment.unwrap_err() == Status::InvArgs;
        if (fragment) {
            PmNode::free_frames(&*fragment);
        }
        auto const after_fragment = PmNode::alloc_frames(nid, flags, 0, mask);
        auto const fragment_restored_last = after_fragment && *after_fragment == last;
        if (after_fragment) {
            PmNode::free_frames(*after_fragment, 0);
        }
        PmNode::free_frames(&held);

        verdict(tally, drained && fell_back,
                "allocation falls back after exhausting the requested node");
        verdict(tally, rejected && restored_last, "a partial bulk allocation returns its page");
        verdict(tally, invalid && fragment_restored_last,
                "a partial fragment allocation returns its page");
    }

    auto check_remote_exhaustion(Tally &tally) -> void {
        // Run from an idle guest. Target a memory-only node so CPUs retain their local memory.
        auto const home = mm::current_node();
        for (NodeId nid = 0; nid < MAX_NODES; ++nid) {
            if (nid != home && PmNode::node_memory_mask().test(nid) &&
                !mm::node_work_cpus(nid).any()) {
                check_exhausted_node(tally, nid);
                return;
            }
        }
        verdict(tally, false, "remote exhaustion needs a memory-only node");
    }

    auto check_address_helpers(Tally &tally) -> void {
        auto const flags = mm::ALLOC_KERNEL;
        auto addr = PhysAddr::Invalid;
        auto *const frame = mm::alloc_frames(flags, &addr, 0);
        verdict(tally, frame && addr == mm::frame_to_phys(frame),
                "allocation writes the physical address of the returned frame");
        if (frame) {
            mm::free_frames(mm::frame_to_virt(frame), 0);
        }

        auto *const without_addr = mm::alloc_frames(flags, static_cast<PhysAddr *>(nullptr), 0);
        verdict(tally, without_addr != nullptr, "the physical address output is optional");
        if (without_addr) {
            mm::free_frames(without_addr, 0);
        }

        addr = PhysAddr::Invalid;
        auto *const invalid = mm::alloc_frames(flags, &addr, MAX_FRAME_ORDER + 1);
        verdict(tally, !invalid && addr == PhysAddr::Invalid,
                "failed allocation preserves the physical address output");
        if (invalid) {
            mm::free_frames(invalid, invalid->order());
        }
    }

    auto check_count_helpers(Tally &tally) -> void {
        auto const check = [&tally](u8 *const bytes, NodeId const expected_node,
                                    char const *const name) {
            if (!bytes) {
                verdict(tally, false, name);
                return;
            }

            auto *const frame = mm::virt_to_frame(bytes);
            auto const order = frame->order();
            auto ok = order == 2;
            if (ok) {
                constexpr auto NR_FRAMES = usize{4};
                constexpr auto SIZE = NR_FRAMES * PAGE_SIZE;
                ok = mm::frame_to_pfn(frame) % NR_FRAMES == 0 &&
                     std::all_of(bytes, bytes + SIZE, [](u8 const value) { return value == 0; });
                if (expected_node < MAX_NODES) {
                    ok = ok && frame->nid() == expected_node;
                }
                bytes[SIZE - 1] = 0x5a;
            }

            // Release the actual allocation even if its geometry was wrong.
            mm::free_frames(frame, order);
            verdict(tally, ok, name);
        };

        auto const alloc_flags = mm::ALLOC_KERNEL | mm::AllocFlags::Zero;
        check(mm::get_frames_n<u8>(alloc_flags, 3), MAX_NODES,
              "get_frames_n() returns a usable zeroed block of at least three pages");
        auto const nid = mm::current_node();
        check(mm::get_frames_n<u8>(nid, alloc_flags | mm::AllocFlags::HomeIfAllowed, 3,
                                   NodeMask::of(nid)),
              nid, "node-specific get_frames_n() honors the node and rounds the count");
    }

    auto check_disabled_allocation(Tally &tally) -> void {
        auto &state = task::Thread::Current::alloc_state();

        state.disable();
        auto *const nodeless = mm::alloc_frames(mm::ALLOC_KERNEL, usize(0));
        auto *const by_node = mm::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL, usize(0));
        auto direct = PmNode::alloc_frames(mm::ALLOC_KERNEL);
        auto direct_home = PmNode::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL);
        auto const table = mm::alloc_page_table(PAGE_SIZE, PAGE_SIZE);
        auto *const bytes = mm::get_frames<u8>(mm::ALLOC_KERNEL);
        auto *const local_bytes = mm::get_frames<u8>(mm::current_node(), mm::ALLOC_KERNEL);
        state.enable();

        auto *const after = mm::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL, usize(0));
        verdict(tally, !nodeless && !by_node,
                "a thread that disabled allocation is refused with or without a node");
        verdict(tally, after != nullptr, "enabling it again lets the allocation through");
        verdict(tally, table == PhysAddr{0}, "page-table allocation respects disabled allocation");
        if (table != PhysAddr{0}) {
            mm::free_page_table(table, PAGE_SIZE);
        }
        verdict(tally, !bytes && !local_bytes, "typed allocation returns nullptr when disabled");
        for (auto *const ptr: {bytes, local_bytes}) {
            if (ptr) {
                mm::free_frames(mm::virt_to_frame(ptr), 0);
            }
        }
        verdict(tally,
                !direct && direct.unwrap_err() == Status::AccessDenied && !direct_home &&
                    direct_home.unwrap_err() == Status::AccessDenied,
                "static allocation enforces the thread's disabled state with or without a home");
        for (auto const &result: {direct, direct_home}) {
            if (result && *result) {
                PmNode::free_frames(*result, (*result)->order());
            }
        }

        // Freed even if the check above failed, so the test leaks nothing.
        for (auto *const frame: {nodeless, by_node, after}) {
            if (frame) {
                mm::free_frames(frame, 0);
            }
        }
    }

    auto pmnode_test_command(int argc, char **argv) -> int {
        if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--exhaust-remote")) {
            log::error("usage: pmnode.test [--exhaust-remote]");
            return 1;
        }
        Tally tally;

        run_topology_checks(tally);
        PmNode::for_each_node(PmNode::State::Online, [&tally](PmNode &node) {
            log::info("pmnode.test: node {}", node.nid());
            check_zone_list(tally, node);
            check_fallback_orders(tally, node.nid());
            check_zone_selector(tally, node);
            check_contains(tally, node);
        });
        check_fragment_and_bulk(tally);
        check_batch_errors(tally);
        check_node_queries(tally);
        if (argc == 2) {
            check_remote_exhaustion(tally);
        }
        check_count_helpers(tally);
        check_address_helpers(tally);
        check_static_allocation(tally);
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
