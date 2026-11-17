/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// PmNode.MatchesTheMachineItRunsOn: checks the physical-memory node layer on
/// the machine it runs on -- whatever node layout that is, one node or
/// several.
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
///   - ZoneCandidates, which every allocation now walks: its three filters
///     (holes in the by-type array, the request's node mask, the request's
///     zone-type range in both directions) and Gaf::ThisNode, which is served
///     by resolve_request() narrowing the mask rather than by switching lists.
///     These cases were written against ZoneIterator and AllocationContext,
///     which were on no allocation path and are gone; they test the real one
///     now, almost unchanged.
///   - contains(). Declared for a long time and defined only recently.
///   - Fragment and bulk allocation. alloc_frames_fragment() used to loop for
///     ever on an even page count. Fallback from an exhausted node and the
///     list forms refused at its floor are PmNode.FallsBackFromAnExhaustedRemoteNode,
///     a test of its own, run with the exhaust-remote option from an idle
///     guest with a memory-only NUMA node.
///   - Node queries. num_nodes() agrees with the ids find_node() answers, and
///     the current node's queries describe it. What an absent node id
///     answers is PmNode.AbsentNodesAnswerNothing, skipped where every id is
///     in use.
///   - Home allocation. An explicit home, Gaf::ThisNode and an order past the
///     maximum through mm::alloc_frames(); an empty node set and one without
///     the home through mm::priv::alloc_frames(), since no public function
///     takes a node set.
///   - Count helpers. get_frames_n() rounds three pages up to a usable,
///     zeroed four-page block, asked once with no home and once with one.
///   - AllocState. A thread that disabled allocation must be
///     refused on the node-less path and on the path that names a node alike.
///
/// Why an in-kernel test: PmNode is built on EarlyMem and the buddy, which
/// exist only in a booted kernel.
#include <ours/cpu.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/alloc_state.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/memory_watchdog.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/mm/priv/pmm.hpp>
#include <ours/mm/priv/zone_candidates.hpp>
#include <ours/node_mask.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <ktest/ktest.hpp>
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

    /// The descriptor of the counter called |name|, or null when no counter
    /// of that name is linked into this kernel.
    [[nodiscard]]
    auto find_counter(std::string_view const name) -> gktl::Counter::Descriptor const * {
        using Descriptor = gktl::Counter::Descriptor;
        auto const *const found = std::find_if(
            Descriptor::GLOBAL_DESC_START, Descriptor::GLOBAL_DESC_END,
            [name](Descriptor const &each) { return std::string_view{each.name} == name; });
        if (found == Descriptor::GLOBAL_DESC_END) {
            return nullptr;
        }
        return found;
    }

    /// How far |counter| moved while |act| ran. A counter keeps one slot per
    /// CPU and get() reads the calling CPU's, so both reads and |act| between
    /// them run with interrupts off: nothing preempts the thread or moves it
    /// to another CPU, whose slot would answer the second read instead.
    template <typename F>
    [[nodiscard]]
    auto count_around(gktl::Counter const &counter, F &&act) -> isize {
        arch::AutoInterruptDisabler irq;
        auto const before = counter.get();
        std::forward<F>(act)();
        return counter.get() - before;
    }

    auto run_topology_checks() -> void {
        // The CPU and its node are two reads, and the test is that the second
        // answers for the CPU the first named. This thread is not pinned, and an
        // idle CPU can take it between the reads, so they are made with
        // interrupts off, as check_local_fallback() does. Only these two need
        // it: every other mm::current_node() in this file is used as a node
        // number the test then passes on, never compared with a CPU.
        auto cpu = CpuNum{};
        auto nid = NodeId{};
        {
            arch::AutoInterruptDisabler irq;
            cpu = mm::CpuLocal::current_cpu();
            nid = mm::current_node();
        }
        log::info("pmnode:   cpu{} is on node {}; {} node(s) online", cpu, nid,
                  mm::num_online_nodes());

        EXPECT_TRUE(nid < MAX_NODES && mm::find_node(nid) != nullptr,
                    "the calling CPU resolves to a node that exists");
        // There is one current_node() now. It used to be three -- a
        // NumaTopology::current_node(), mm::current_node() wrapping it, and
        // the table read -- and this case compared all three. With the
        // topology's own gone, what is left to check is that the wrapper
        // really reads the placement table for the calling CPU rather than
        // answering from somewhere else.
        EXPECT_EQ(nid, mm::CpuNodeMap::node_of(cpu),
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
                auto const placed = mm::CpuNodeMap::node_of(each);
                if (mm::find_node(declared) == nullptr) {
                    memoryless_cpus += 1;
                    if (placed >= MAX_NODES || mm::find_node(placed) == nullptr) {
                        rehomed = false;
                    }
                    return;
                }
                if (placed != declared) {
                    unmoved = false;
                }
            });
        log::info("pmnode:   {} of {} possible CPU(s) were placed by firmware", declared_cpus,
                  Cpu::possible_mask().count());
        if (memoryless_cpus != 0) {
            log::info("pmnode:   {} of them on a node with no memory", memoryless_cpus);
        }
        EXPECT_TRUE(unmoved,
                    "no CPU the firmware placed on a node that exists has been moved off it");
        EXPECT_TRUE(rehomed,
                    "every CPU the firmware placed on a node with no memory runs from one that "
                    "exists");

        auto const online = mm::num_online_nodes();
        EXPECT_TRUE(online >= 1 && online == mm::node_online_mask().count(),
                    "num_online_nodes() counts the Online mask");
    }

    /// The checks one node's ZoneList must pass; see the file comment.
    auto check_zone_list(PmNode &node) -> void {
        auto const nid = node.nid();
        auto const &list = node.zone_list();

        // By type: every non-null slot holds a zone of that type on this node,
        // and for_each_local_zone() visits exactly those, in type order.
        auto by_type_ok = true;
        usize local_count = 0;
        for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
            auto const type = ZoneType(i);
            if (auto *const zone = list.local_zone(type)) {
                local_count += 1;
                if (zone->which_node() != nid || zone->zone_type() != type) {
                    by_type_ok = false;
                }
            }
        }
        usize visited = 0;
        auto last_type = usize(0);
        list.for_each_local_zone([&](PmZone &zone) {
            auto const type = std::to_underlying(zone.zone_type());
            if (visited != 0 && type <= last_type) {
                by_type_ok = false;
            }
            if (list.local_zone(zone.zone_type()) != &zone) {
                by_type_ok = false;
            }
            last_type = type;
            visited += 1;
        });
        EXPECT_TRUE(by_type_ok && visited == local_count && local_count >= 1,
                    "a node's local zones are its own, one per type, walked in type order");

        // Fallback order: own zones first, from the highest type down.
        auto const fallback = list.fallback();
        auto head_ok = fallback.size() >= local_count;
        auto index = local_count;
        list.for_each_local_zone([&](PmZone &zone) {
            index -= 1;
            if (index >= fallback.size() || fallback[index] != &zone) {
                head_ok = false;
            }
        });
        EXPECT_TRUE(head_ok, "a node's fallback order starts with its own zones, highest first");

        // Every node's zones form one run, and within a run the types fall.
        // A node seen again after another node's run would mean its zones
        // were split, and a type that rises within a run would mean a low
        // zone is tried before a higher one of the same node.
        auto runs_ok = true;
        NodeMask finished;
        for (usize i = 0; i < fallback.size(); ++i) {
            auto const here = fallback[i]->which_node();
            if (i == 0 || fallback[i - 1]->which_node() != here) {
                if (finished.test(here)) {
                    runs_ok = false;
                }
                if (i != 0) {
                    finished.set(fallback[i - 1]->which_node());
                }
                continue;
            }
            if (fallback[i]->zone_type() >= fallback[i - 1]->zone_type()) {
                runs_ok = false;
            }
        }
        EXPECT_TRUE(runs_ok, "each node's zones are one run in the fallback order, highest first");

        // No zone twice, every zone on an online node.
        auto unique_ok = true;
        for (usize i = 0; i < fallback.size(); ++i) {
            if (!mm::node_online_mask().test(fallback[i]->which_node())) {
                unique_ok = false;
            }
            for (usize j = i + 1; j < fallback.size(); ++j) {
                if (fallback[i] == fallback[j]) {
                    unique_ok = false;
                }
            }
        }
        EXPECT_TRUE(unique_ok, "no zone appears twice in a fallback order");

        // The walk the list offers visits the same zones in the same order,
        // and stops at the first zone a callback returns.
        usize walked = 0;
        auto walk_ok = true;
        list.for_each_zone([&](PmZone &zone) {
            walk_ok = walk_ok && walked < fallback.size() && fallback[walked] == &zone;
            walked += 1;
        });
        auto const *const first = list.for_each_zone([](PmZone &zone) { return &zone; });
        EXPECT_TRUE(walk_ok && walked == fallback.size() &&
                        (fallback.size() == 0 || first == fallback[0]),
                    "for_each_zone walks the fallback order and stops at the first answer");

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
        EXPECT_TRUE(distance_ok, "remote zones follow, nearest node first");
    }

    auto check_fallback_orders(NodeId const nid) -> void {
        auto const &online = mm::node_online_mask();
        auto const members = online & mm::node_memory_mask();

        // Static: MAX_NODES rows is kilobytes on a NUMA build, and a ktest
        // runs on an ordinary thread stack. Single-threaded, like the rest of
        // this test.
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
        EXPECT_TRUE(route_ok && hops == expected,
                    "build_fallback_orders() lists every other node with memory once, nearest "
                    "first");

        // No members, so nothing can be a hop -- but every node in the first
        // set still gets a row, and here every row must be empty.
        //
        // The begin()/end() comparison this replaced was made on two separate
        // temporaries, which are never equal except by the accident of the two
        // landing at the same stack address -- true on one machine, false on
        // the next. One table, so the row compared is the row filled.
        numa::build_fallback_orders(online, NodeMask{}, &orders);
        EXPECT_TRUE(orders[nid].begin() == orders[nid].end(),
                    "build_fallback_orders() with no members leaves an empty order");
    }

    auto count_selected(mm::ZoneCandidates const &selector) -> usize {
        usize count = 0;
        for (auto it = selector.begin(); it != selector.end(); ++it) {
            count += 1;
        }
        return count;
    }

    /// A request over every online node, which each case then narrows.
    auto request_for(PmNode &node, mm::Gaf const alloc_flags) -> mm::AllocRequest {
        return mm::resolve_request(alloc_flags, 0, mm::node_online_mask(),
                                   mm::AllocRequest::Origin{.node = node.nid(), .cpu = Cpu::id()});
    }

    auto check_zone_candidates(PmNode &node) -> void {
        auto const &list = node.zone_list();

        usize local_count = 0;
        list.for_each_local_zone([&local_count](PmZone &) { local_count += 1; });

        usize dma_count = 0;
        for (auto *const zone: list.fallback()) {
            if (zone->zone_type() == ZoneType::Dma) {
                dma_count += 1;
            }
        }

        // Holes: the by-type array has a null for each missing type, and the
        // selector must step over them rather than stop or dereference one.
        auto const all = request_for(node, mm::ALLOC_KERNEL);
        mm::ZoneCandidates const holes{list.local_zones(), all};
        EXPECT_EQ(count_selected(holes), local_count,
                  "ZoneCandidates steps over the holes in the by-type array");

        // Node filter: excluding this node leaves only the remote zones.
        auto without_local = all.allowed_nodes();
        without_local.set(node.nid(), false);
        auto const others = all.with_nodes(without_local);
        auto const remote_walk = list.candidates(others);
        auto remote_ok = true;
        usize remote = 0;
        for (auto &zone: remote_walk) {
            if (zone.which_node() == node.nid()) {
                remote_ok = false;
            }
            remote += 1;
        }
        EXPECT_TRUE(remote_ok && remote == list.fallback().size() - local_count,
                    "ZoneCandidates's node mask filters out the excluded node");

        // Upper bound: Dma as the ceiling yields only Dma zones.
        auto const low_only = all.with_zone_range(all.lowest_zone(), ZoneType::Dma);
        auto const low_walk = list.candidates(low_only);
        auto upper_ok = true;
        usize dma = 0;
        for (auto &zone: low_walk) {
            if (zone.zone_type() != ZoneType::Dma) {
                upper_ok = false;
            }
            dma += 1;
        }
        EXPECT_TRUE(upper_ok && dma == dma_count,
                    "ZoneCandidates's upper bound filters out higher zone types");

        // Lower bound, which the walk this replaced stored and never read: a
        // floor of Dma32 must keep a caller off the Dma zones entirely. This
        // is the case that could not have been written before.
        auto const high_only = all.with_zone_range(ZoneType::Dma32, all.highest_zone());
        auto const high_walk = list.candidates(high_only);
        auto lower_ok = true;
        usize above = 0;
        for (auto &zone: high_walk) {
            if (zone.zone_type() < ZoneType::Dma32) {
                lower_ok = false;
            }
            above += 1;
        }
        EXPECT_TRUE(lower_ok && above == list.fallback().size() - dma_count,
                    "ZoneCandidates's lower bound keeps a caller off the zones below it");

        // Gaf::ThisNode, through the path that serves it: resolve_request()
        // narrows the request's node mask, so the selector over the whole
        // fallback order yields this node's zones and no others. The walk this
        // replaced switched between two candidate lists instead, which is a
        // second mechanism for the same intent.
        auto const this_node = request_for(node, mm::ALLOC_KERNEL | mm::Gaf::ThisNode);
        auto const local_walk = list.candidates(this_node);
        auto this_node_ok = true;
        usize local_selected = 0;
        for (auto &zone: local_walk) {
            if (zone.which_node() != node.nid()) {
                this_node_ok = false;
            }
            local_selected += 1;
        }
        log::info("pmnode:   ThisNode selected {} of {} zone(s) in the fallback order",
                  local_selected, list.fallback().size());
        EXPECT_TRUE(this_node_ok && local_selected == local_count,
                    "Gaf::ThisNode narrows the request's node mask, and the selector "
                    "honours it");
    }

    auto check_contains(PmNode &node) -> void {
        auto ok = true;
        for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
            auto const type = ZoneType(i);
            auto *const zone = node.zone_list().local_zone(type);
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
        EXPECT_TRUE(ok,
                    "contains() follows the zone, says no without one, and no past the top order");
    }

    auto check_fragment_and_bulk() -> void {
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
            log::info("pmnode:   alloc_frames_fragment({}) -> {} block(s), {} page(s)", n, blocks,
                      pages);
            if (pages != n || blocks != static_cast<usize>(ustl::popcount(n))) {
                fragment_ok = false;
            }
            mm::free_frames(&*result);
        }
        EXPECT_TRUE(fragment_ok,
                    "alloc_frames_fragment() returns one block per set bit, n pages in all");

        auto bulk = mm::alloc_frames_bulk(mm::ALLOC_KERNEL, 4);
        auto bulk_ok = !!bulk;
        PmFrame::List list;
        if (bulk) {
            list = ustl::move(*bulk);
        }
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
        EXPECT_TRUE(bulk_ok && frames == 4, "alloc_frames_bulk() returns n single frames");
    }

    auto check_home_allocation() -> void {
        auto const release = [](ktl::Result<PmFrame *> const &result) {
            if (result && *result) {
                mm::free_frames(*result);
            }
        };
        auto const alloc_flags = mm::ALLOC_KERNEL | mm::Gaf::Zero;
        auto const nid = mm::current_node();

        auto implicit = mm::alloc_frames(alloc_flags);
        EXPECT_TRUE(implicit && *implicit && (*implicit)->order() == 0,
                    "an allocation that names no home starts on the local node");
        release(implicit);

        auto local = mm::alloc_frames(alloc_flags | mm::Gaf::ThisNode);
        EXPECT_TRUE(local && *local, "ThisNode also accepts the implicit home");
        release(local);

        auto explicit_home = mm::alloc_frames(alloc_flags | mm::Gaf::ThisNode, 0, nid);
        EXPECT_TRUE(explicit_home && *explicit_home && (*explicit_home)->nid() == nid,
                    "an allocation honors an explicit home and ThisNode");
        release(explicit_home);

        auto empty = mm::priv::alloc_frames(alloc_flags, 0, nid, NodeMask{});
        EXPECT_TRUE(!empty && empty.unwrap_err() == Status::OutOfMem,
                    "an allocation cannot escape an empty allowed-node set");
        release(empty);

        auto oversized = mm::alloc_frames(alloc_flags, MAX_FRAME_ORDER + 1, nid);
        EXPECT_TRUE(!oversized && oversized.unwrap_err() == Status::InvArgs,
                    "an allocation rejects an order past the maximum");
        release(oversized);

        auto remote_nodes = mm::node_memory_mask();
        remote_nodes.set(nid, false);
        if (remote_nodes.any()) {
            auto home_excluded =
                mm::priv::alloc_frames(alloc_flags | mm::Gaf::ThisNode, 0, nid, remote_nodes);
            EXPECT_TRUE(!home_excluded && home_excluded.unwrap_err() == Status::OutOfMem,
                        "ThisNode fails when the allowed mask excludes the requested node");
            release(home_excluded);

            auto remote = mm::priv::alloc_frames(alloc_flags, 0, nid, remote_nodes);
            EXPECT_TRUE(remote && *remote && remote_nodes.test((*remote)->nid()),
                        "an allocation reaches another node permitted by the mask");
            release(remote);
        } else {
            log::info("pmnode: skip  cross-node allocation needs multiple memory nodes");
        }
    }

    // The low bit allocates a page before the high bit requests an unsupported block.
    constexpr auto PARTIAL_FRAGMENT_COUNT = usize{1} + (usize{1} << (MAX_FRAME_ORDER + 1));

    auto check_batch_errors() -> void {
        auto const nid = mm::current_node();
        auto const flags = mm::ALLOC_KERNEL;
        auto empty_fragment = mm::alloc_frames_fragment(flags, 0);
        auto empty_bulk = mm::alloc_frames_bulk(flags, 0);
        EXPECT_TRUE(empty_fragment && empty_fragment->empty() && empty_bulk && empty_bulk->empty(),
                    "zero-page batch requests return empty lists");
        if (empty_fragment) {
            mm::free_frames(&*empty_fragment);
        }
        if (empty_bulk) {
            mm::free_frames(&*empty_bulk);
        }

        auto fragment =
            mm::alloc_frames_fragment(flags | mm::Gaf::ThisNode, PARTIAL_FRAGMENT_COUNT, nid);
        EXPECT_TRUE(!fragment && fragment.unwrap_err() == Status::InvArgs,
                    "a fragment request fails when a later block exceeds the maximum order");
        if (fragment) {
            mm::free_frames(&*fragment);
        }

        auto bulk = mm::priv::alloc_frames_bulk(flags, 1, nid, NodeMask{});
        EXPECT_TRUE(!bulk && bulk.unwrap_err() == Status::OutOfMem,
                    "the bulk interface reports an exhausted candidate set");
        if (bulk) {
            mm::free_frames(&*bulk);
        }
    }

    auto check_node_queries() -> void {
        auto const nid = mm::current_node();
        auto *const node = mm::find_node(nid);
        // num_nodes() against the node table walked id by id: every id that
        // find_node() answers is one node.
        usize found = 0;
        for (NodeId each = 0; each < MAX_NODES; ++each) {
            if (mm::find_node(each)) {
                found += 1;
            }
        }
        EXPECT_TRUE(node && found == mm::num_nodes() &&
                        mm::node_work_cpus(nid) == node->work_cpus() &&
                        mm::node_soft_affinity(nid) == &node->soft_affinity() &&
                        mm::node_hard_affinity(nid) == &node->hard_affinity(),
                    "PMM topology queries describe the current node");
    }

    auto check_local_fallback(NodeId const remote_home) -> bool {
        auto const *const hit = find_counter("pm.node.local.hit");
        if (!hit) {
            return false;
        }

        // Read one CPU's counter around the attempt without a preemption or
        // interrupt. The local node is read inside that window too, so it is
        // the node the allocation itself took as local.
        gktl::Counter const hits{hit};
        auto local = NodeId{MAX_NODES};
        PmFrame *frame = nullptr;
        auto const counted = count_around(hits, [remote_home, &local, &frame] {
            local = mm::current_node();
            auto nodes = NodeMask::of(remote_home);
            nodes.set(local);
            // No flags: interrupts are off here, so this caller cannot block,
            // and the request must not carry ALLOC_KERNEL's Gaf::CanBlock.
            auto const fallback = mm::priv::alloc_frames(mm::Gaf{}, 0, remote_home, nodes);
            if (fallback) {
                frame = *fallback;
            }
        });
        if (!frame) {
            return false;
        }

        auto const counted_local = frame->nid() == local && counted == 1;
        mm::free_frames(frame);
        return counted_local;
    }

    auto check_zone_inventory(PmZone &zone) -> bool {
        struct Trial {
            PmZone &zone;
            sync::Event start;
        } trial{.zone = zone, .start = {}};

        auto const exercise = [](void *arg) -> i32 {
            auto &state = *static_cast<Trial *>(arg);
            if (state.start.wait(time::Deadline::infinite()) != Status::Ok) {
                return 1;
            }
            constexpr auto ITERATIONS = usize{8192};
            for (usize i = 0; i < ITERATIONS; ++i) {
                auto const block = state.zone.alloc_frames(0);
                auto const count = state.zone.num_free_frames();
                if (block) {
                    // Back into the zone it came from, below the pipeline:
                    // no waiter to wake.
                    auto _ = state.zone.free_frames(block.value());
                }
                if (count > 1 || state.zone.num_free_frames() > 1) {
                    return 1;
                }
            }
            return 0;
        };

        std::array<task::Thread *, 2> workers{};
        usize started = 0;
        for (CpuNum cpu = 0; cpu < MAX_CPU && started < workers.size(); ++cpu) {
            if (!Cpu::is_online(cpu)) {
                continue;
            }
            auto *const worker =
                task::Thread::spawn("pmnode-inventory", exercise, &trial, task::DEFAULT_PRIORITY);
            if (!worker) {
                break;
            }
            worker->set_cpu_affinity_hard(CpuMask::of(cpu));
            auto _ = worker->resume();
            workers[started++] = worker;
        }
        trial.start.signal();

        auto ok = started == workers.size();
        for (auto *const worker: workers) {
            if (worker) {
                i32 retcode = 0;
                if (worker->join(&retcode, time::Deadline::infinite()) != Status::Ok ||
                    retcode != 0) {
                    ok = false;
                }
            }
        }
        return ok && zone.num_free_frames() == 1 && zone.count_free_frames() == 1;
    }

    auto check_exhausted_node(NodeId const nid) -> void {
        auto const flags = mm::ALLOC_KERNEL | mm::Gaf::ThisNode;
        auto const reserved = mm::alloc_frames(flags, 0, nid);
        if (!reserved) {
            FAIL("reserve a page on the memory-only node");
        }

        // Through mm:: until the node refuses, which is at its zones' Min
        // floor now, and then through the zones themselves, which have no
        // floor, so that the node is empty for the checks below.
        PmFrame::List held;
        for (usize order = MAX_FRAME_ORDER + 1; order > 0; --order) {
            while (auto block = mm::alloc_frames(flags, order - 1, nid)) {
                held.push_back(**block);
            }
        }
        mm::find_node(nid)->zone_list().for_each_local_zone([&held](PmZone &zone) {
            for (usize order = MAX_FRAME_ORDER + 1; order > 0; --order) {
                while (auto block = zone.alloc_frames(order - 1)) {
                    held.push_back(block.value());
                }
            }
        });
        auto const this_node = mm::alloc_frames(flags, 0, nid);
        auto const this_node_failed = !this_node && this_node.unwrap_err() == Status::OutOfMem;
        if (this_node) {
            mm::free_frames(*this_node);
        }
        auto const counted_local = check_local_fallback(nid);

        auto *const last = *reserved;
        auto *const zone = PmZone::at(nid, last->zone());
        auto const drained = zone && zone->count_free_frames() == 0 &&
                             zone->num_free_frames() == 0 &&
                             !zone->watermark_ok(0, PmZone::Watermark::Min);
        mm::free_frames(last);
        if (Cpu::num_online_cpus() >= 2) {
            EXPECT_TRUE(drained && check_zone_inventory(*zone),
                        "two CPUs keep an exhausted zone's free count within its one-page "
                        "inventory");
        }
        // One frame free and a floor above it: every list form is refused on
        // the first block, and the one free frame stays where it is. Whether
        // a rollback returns the very frame it took is no longer reachable
        // here, since nothing past the floor can be taken through mm::; the
        // batch errors on a healthy node cover the rollback itself.
        auto const one_free = [zone] {
            return zone && zone->num_free_frames() == 1 && zone->count_free_frames() == 1;
        };
        auto bulk = mm::alloc_frames_bulk(flags, 2, nid);
        auto const rejected = !bulk && bulk.unwrap_err() == Status::OutOfMem;
        if (bulk) {
            mm::free_frames(&*bulk);
        }
        auto const restored_last = one_free();

        auto fragment = mm::alloc_frames_fragment(flags, PARTIAL_FRAGMENT_COUNT, nid);
        auto const fragment_refused = !fragment && fragment.unwrap_err() == Status::OutOfMem;
        if (fragment) {
            mm::free_frames(&*fragment);
        }
        auto const fragment_restored_last = one_free();
        mm::free_frames(&held);

        EXPECT_TRUE(this_node_failed, "ThisNode fails after exhausting the requested node");
        EXPECT_TRUE(drained && counted_local,
                    "fallback from the exhausted remote home supplies and counts a local page");
        EXPECT_TRUE(rejected && restored_last,
                    "a bulk allocation refused at the floor leaves the free frame in place");
        EXPECT_TRUE(fragment_refused && fragment_restored_last,
                    "a fragment allocation refused at the floor leaves the free frame in place");
    }

    auto check_remote_exhaustion() -> void {
        // Run from an idle guest. Target a memory-only node so CPUs retain their local memory.
        auto const home = mm::current_node();
        for (NodeId nid = 0; nid < MAX_NODES; ++nid) {
            if (nid != home && mm::node_memory_mask().test(nid) && !mm::node_work_cpus(nid).any()) {
                mm::MemoryReclaimer::Pause pause{mm::MemoryReclaimer::get()};
                check_exhausted_node(nid);
                return;
            }
        }
        KTEST_SKIP("remote exhaustion needs a memory-only node");
    }

    auto check_count_helpers() -> void {
        auto const check = [](u8 *const bytes, NodeId const expected_node, char const *const name) {
            if (!bytes) {
                FAIL("{}", name);
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
            mm::free_frames(frame);
            EXPECT_TRUE(ok, "{}", name);
        };

        auto const alloc_flags = mm::ALLOC_KERNEL | mm::Gaf::Zero;
        check(mm::get_frames_n<u8>(alloc_flags, 3), MAX_NODES,
              "get_frames_n() returns a usable zeroed block of at least three pages");
        auto const nid = mm::current_node();
        check(mm::get_frames_n<u8>(alloc_flags | mm::Gaf::ThisNode, 3, nid), nid,
              "node-specific get_frames_n() honors the node and rounds the count");
    }

    /// Allocation disabled for the calling thread while one lives, and enabled
    /// again after, however the scope ends, so that a check cannot leave the
    /// thread unable to allocate.
    class AllocationsDisabled {
      public:
        explicit AllocationsDisabled(mm::AllocState &state) : m_state(state) {
            m_state.disable();
        }

        ~AllocationsDisabled() {
            m_state.enable();
        }

        AllocationsDisabled(AllocationsDisabled const &) = delete;
        auto operator=(AllocationsDisabled const &) -> AllocationsDisabled & = delete;

      private:
        mm::AllocState &m_state;
    };

    auto check_disabled_allocation() -> void {
        // What each allocation answered while allocation was disabled. The
        // lambda is the scope that holds it disabled: everything it returns
        // was asked for inside, and allocation is enabled again once it does.
        struct Refused {
            ktl::Result<PmFrame *> nodeless;
            ktl::Result<PmFrame *> by_node;
            PhysAddr table;
            u8 *bytes;
            u8 *local_bytes;
        };
        auto const [nodeless, by_node, table, bytes, local_bytes] = [] {
            AllocationsDisabled const disabled{task::Thread::Current::alloc_state()};
            return Refused{
                .nodeless = mm::alloc_frames(mm::ALLOC_KERNEL),
                .by_node = mm::alloc_frames(mm::ALLOC_KERNEL, 0, mm::current_node()),
                .table = mm::alloc_page_table(PAGE_SIZE, PAGE_SIZE),
                .bytes = mm::get_frames<u8>(mm::ALLOC_KERNEL),
                .local_bytes = mm::get_frames<u8>(mm::ALLOC_KERNEL, 0, mm::current_node()),
            };
        }();

        auto const after = mm::alloc_frames(mm::ALLOC_KERNEL, 0, mm::current_node());
        EXPECT_TRUE(!nodeless && nodeless.unwrap_err() == Status::AccessDenied && !by_node &&
                        by_node.unwrap_err() == Status::AccessDenied,
                    "an allocation reports AccessDenied with or without an explicit node");
        EXPECT_TRUE(!!after, "enabling it again lets the allocation through");
        EXPECT_TRUE(table == PhysAddr{0}, "page-table allocation respects disabled allocation");
        if (table != PhysAddr{0}) {
            mm::free_page_table(table, PAGE_SIZE);
        }
        EXPECT_TRUE(!bytes && !local_bytes, "typed allocation returns nullptr when disabled");
        for (auto *const ptr: {bytes, local_bytes}) {
            if (ptr) {
                mm::free_frames(mm::virt_to_frame(ptr));
            }
        }

        // Freed even if the check above failed, so the test leaks nothing.
        for (auto const &result: {nodeless, by_node, after}) {
            if (result) {
                mm::free_frames(*result);
            }
        }
    }

    KTEST(PmNode, SplitBlockMakesBlocksThatAreFreedAlone) {
        constexpr auto WHOLE = usize{3};
        constexpr auto NR_FRAMES = usize{1} << WHOLE;

        auto const allocated = mm::alloc_frames(mm::ALLOC_KERNEL, WHOLE);
        if (!allocated) {
            FAIL("allocate a block of order {}", WHOLE);
        }
        auto *const block = *allocated;
        EXPECT_TRUE(block->order() == WHOLE, "the head of an allocated block records its order");

        // Four blocks of two frames: every other frame is a head of order 1.
        mm::split_block(block, 1);
        auto halves_ok = true;
        for (usize index = 0; index < NR_FRAMES; index += 2) {
            halves_ok = halves_ok && block[index].order() == 1;
        }
        EXPECT_TRUE(halves_ok, "split_block(block, 1) makes four heads of order 1");

        // The first of them again, into two single frames; the others stay.
        mm::split_block(block);
        EXPECT_TRUE(block[0].order() == 0 && block[1].order() == 0 && block[2].order() == 1,
                    "splitting one of the parts leaves the others as they were");

        // Each part is a block of its own now and goes back alone: five frees
        // for what was one allocation. The EXPECTs above are what check the
        // split. The frees here would not: the buddy asserts only that a block
        // is aligned to its order, so a part returned at a wrong order that is
        // still aligned goes through, and the frame ends up free twice. On a
        // node quiet enough to read its zone's profile around them,
        // ABlockCarriesItsLayoutAndCoalescesBack checks the frees too.
        mm::free_frames(&block[0]);
        mm::free_frames(&block[1]);
        for (usize index = 2; index < NR_FRAMES; index += 2) {
            mm::free_frames(&block[index]);
        }
    }

    KTEST(PmNode, ContiguousFramesAreAdjacentAlignedAndFreedAlone) {
        constexpr auto NR_FRAMES = usize{3};
        // Four pages: more than the three frames need, so the alignment is
        // what decides the block.
        constexpr auto ALIGN_LOG2 = usize{PAGE_SHIFT + 2};

        auto run = mm::alloc_frames_contiguous(mm::ALLOC_KERNEL, NR_FRAMES, ALIGN_LOG2);
        if (!run) {
            FAIL("allocate {} contiguous frames", NR_FRAMES);
        }

        usize count = 0;
        auto ok = true;
        auto expected = PhysAddr{};
        for (auto &frame: *run) {
            auto const phys = mm::frame_to_phys(&frame);
            if (count == 0) {
                ok = ok && (static_cast<usize>(phys) & ((usize{1} << ALIGN_LOG2) - 1)) == 0;
            } else {
                ok = ok && phys == expected;
            }
            ok = ok && frame.order() == 0;
            expected = phys + PAGE_SIZE;
            count += 1;
        }
        EXPECT_TRUE(ok && count == NR_FRAMES,
                    "the run is {} single frames, adjacent, the first aligned", NR_FRAMES);
        mm::free_frames(&*run);

        auto none = mm::alloc_frames_contiguous(mm::ALLOC_KERNEL, 0, PAGE_SHIFT);
        EXPECT_TRUE(!none && none.unwrap_err() == Status::InvArgs, "a run of no frames is refused");
        if (none) {
            mm::free_frames(&*none);
        }

        auto huge =
            mm::alloc_frames_contiguous(mm::ALLOC_KERNEL, 1, PAGE_SHIFT + MAX_FRAME_ORDER + 1);
        EXPECT_TRUE(!huge && huge.unwrap_err() == Status::InvArgs,
                    "an alignment no block has is refused");
        if (huge) {
            mm::free_frames(&*huge);
        }
    }

    KTEST(PmNode, PinFrameCountsWhateverTheRole) {
        auto const allocated = mm::alloc_frames(mm::ALLOC_KERNEL);
        if (!allocated) {
            FAIL("allocate a frame to pin");
        }
        // Nobody else knows this frame, so there is no owner's lock to take:
        // the test is its owner.
        auto *const frame = *allocated;
        EXPECT_TRUE(frame->pin_count() == 0, "a frame just allocated holds no pin");

        auto const first = mm::pin_frame(frame);
        auto const second = mm::pin_frame(frame);
        EXPECT_TRUE(first == Status::Ok && second == Status::Ok && frame->pin_count() == 2,
                    "pins count, on a frame with no role");

        // A role is put on over the pins and does not forget them.
        frame->become<mm::page::MmuBody>();
        EXPECT_TRUE(frame->pin_count() == 2, "a change of role keeps the pins");
        frame->stamp_role(mm::page::Role::None);

        auto const undone = mm::unpin_frame(frame);
        auto const undone_again = mm::unpin_frame(frame);
        EXPECT_TRUE(undone == Status::Ok && undone_again == Status::Ok && frame->pin_count() == 0,
                    "each unpin undoes one pin");
        EXPECT_TRUE(mm::unpin_frame(frame) == Status::BadState,
                    "a frame with no pin cannot be unpinned");

        // With no pin left it may go back; a debug build asserts if it could not.
        mm::free_frames(frame);
    }

    KTEST(PmNode, PageTablesAreNotTheFrameFailureSite) {
#if MM_TEST_ALLOC_FAILURE
        using mm::test::AllocationFailure;
        using mm::test::AllocationSite;

        // A page table is its own failure site. A scope armed for the frame
        // site lets a page table through and fails the next frame asked for
        // with no home, which is what a test counting frame allocations
        // relies on.
        AllocationFailure failure(AllocationSite::Frame);
        auto const table = mm::alloc_page_table(PAGE_SIZE, PAGE_SIZE);
        auto const untouched = !failure.triggered();
        auto const frame = mm::alloc_frames(mm::ALLOC_KERNEL);
        EXPECT_TRUE(table != PhysAddr{0} && untouched,
                    "a page-table allocation does not use up the frame site");
        EXPECT_TRUE(!frame && frame.unwrap_err() == Status::OutOfMem && failure.triggered(),
                    "the next frame with no home is the one that fails");
        if (table != PhysAddr{0}) {
            mm::free_page_table(table, PAGE_SIZE);
        }
        if (frame) {
            mm::free_frames(*frame);
        }
#else
        KTEST_SKIP("needs a build with mm_test_alloc_failure");
#endif
    }

    /// A node other than the calling CPU's with memory in one zone, or null.
    /// Draining it starves neither this test's node nor the zone order of any
    /// allocation that does not name it.
    auto remote_single_zone_node() -> PmNode * {
        auto const local = mm::current_node();
        PmNode *found = nullptr;
        mm::for_each_node(mm::NodeState::Memory, [local, &found](PmNode &node) {
            if (found || node.nid() == local) {
                return;
            }
            usize zones = 0;
            node.zone_list().for_each_local_zone([&zones](PmZone &) { zones += 1; });
            if (zones == 1) {
                found = &node;
            }
        });
        return found;
    }

    /// One zone held still while a test drains it. The reclaimer is paused,
    /// so it gives the zone nothing back while the test counts; every frame
    /// the test takes is held here, and given back when the trial ends,
    /// however the test ends.
    ///
    /// The frames go back in the destructor's body, which runs before any
    /// member is destroyed, the Pause among them: the zone is whole again
    /// before reclaim may look at it. Not a KTEST_F fixture: a fixture is one
    /// static instance for every test that names it, which would hold the
    /// Pause for good.
    class ZoneTrial {
      public:
        explicit ZoneTrial(PmZone &zone) : m_zone(zone), m_no_reclaim(mm::MemoryReclaimer::get()) {}

        ~ZoneTrial() {
            mm::free_frames(&m_held);
        }

        ZoneTrial(ZoneTrial const &) = delete;
        auto operator=(ZoneTrial const &) -> ZoneTrial & = delete;

        /// Takes frames out of the zone through the zone itself, which has no
        /// floor, until its free count is |target|. Larger blocks first while
        /// they fit, so that a zone of a hundred thousand frames is drained in
        /// a few hundred calls. Whether the count reached |target|.
        auto drain_to(usize const target) -> bool {
            for (usize order = MAX_FRAME_ORDER + 1; order > 0; --order) {
                auto const size = usize{1} << (order - 1);
                while (m_zone.num_free_frames() >= target + size) {
                    auto block = m_zone.alloc_frames(order - 1);
                    if (!block) {
                        break;
                    }
                    m_held.push_back(block.value());
                }
            }
            return m_zone.num_free_frames() == target;
        }

        /// Holds the frame an allocation through the pipeline gave the test,
        /// if it gave one, until the trial ends.
        auto keep(ktl::Result<PmFrame *> const &result) -> void {
            if (result) {
                m_held.push_back(**result);
            }
        }

        /// Hands one held frame back to the test, which gives it back itself;
        /// null when the trial holds none.
        auto hand_over() -> PmFrame * {
            auto front = m_held.front();
            if (!front) {
                return nullptr;
            }
            auto *const frame = &front.value();
            m_held.pop_front();
            return frame;
        }

      private:
        PmZone &m_zone;
        mm::MemoryReclaimer::Pause const m_no_reclaim;
        PmFrame::List m_held;
    };

    KTEST(PmNode, AdmissionHoldsAtLowThenAtMin) {
        // A node other than the calling CPU's, with one zone, so that every
        // allocation below asks exactly one zone and the test does not starve
        // its own node. The four-node machine has three; one node has none.
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];
        auto const min = zone.watermark(PmZone::Watermark::Min);
        auto const low = zone.watermark(PmZone::Watermark::Low);
        if (min < 4 || low <= min + 1) {
            KTEST_SKIP("the zone's watermarks are too close to test between");
        }

        // Reclaim would give frames back to the zone while the test holds its
        // count still. Nothing new is reclaimed while the trial lives; only a
        // walk already under way as it began could still run, and nothing
        // marks this node short beforehand.
        ZoneTrial trial{zone};
        auto const blocking = mm::ALLOC_KERNEL | mm::Gaf::ThisNode;
        auto const non_blocking = mm::Gaf::ThisNode;

        // At Low: the first pass refuses, the second lets a blocking request
        // down to Min.
        auto const at_low = trial.drain_to(low);
        auto const below_low = mm::alloc_frames(blocking, 0, nid);
        auto const second_pass = below_low && (*below_low)->nid() == nid;
        trial.keep(below_low);

        // At Min: a blocking request is refused, one that cannot block is not.
        auto const at_min = trial.drain_to(min);
        auto const refused = mm::alloc_frames(blocking, 0, nid);
        auto const blocking_refused = !refused && refused.unwrap_err() == Status::OutOfMem;
        trial.keep(refused);
        auto const deeper = mm::alloc_frames(non_blocking, 0, nid);
        auto const non_blocking_admitted = deeper && (*deeper)->nid() == nid;
        trial.keep(deeper);

        // At three quarters of Min: nobody.
        auto const at_floor = trial.drain_to(min - min / 4);
        auto const last = mm::alloc_frames(non_blocking, 0, nid);
        auto const non_blocking_refused = !last && last.unwrap_err() == Status::OutOfMem;
        trial.keep(last);

        EXPECT_TRUE(at_low && second_pass,
                    "below Low a blocking request is served by the second pass, down to Min");
        EXPECT_TRUE(at_min && blocking_refused, "at Min a blocking request is refused");
        EXPECT_TRUE(non_blocking_admitted, "at Min a request that cannot block is still served");
        EXPECT_TRUE(at_floor && non_blocking_refused,
                    "at three quarters of Min a request that cannot block is refused too");
    }

    // No thread waits in this test: what it measures is the pm.zone.relief
    // counter, which counts the wake a release would send to waiters, and a
    // waiting request retried on this same thread after the release.
    KTEST(PmNode, WaitingRequestsStopAtLowAndReliefIsCountedOnce) {
        auto const *const relief = find_counter("pm.zone.relief");
        if (!relief) {
            FAIL("the relief counter is linked in");
        }
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];
        auto const low = zone.watermark(PmZone::Watermark::Low);
        auto const min = zone.watermark(PmZone::Watermark::Min);
        if (low <= min + 2) {
            KTEST_SKIP("the zone's watermarks are too close to test between");
        }

        // A request is put off at Low only while the gate is armed, which the
        // watchdog does at boot; armed here by hand, with the watchdog held,
        // so that the test does not depend on whether a watchdog runs or has
        // armed the gate again since the test before. The delay level is one
        // frame, which free memory never reaches, so the gate itself never
        // shuts. When the Hold goes the watchdog looks and arms it as it was.
        mm::MemoryWatchdog::Hold const no_watchdog{mm::MemoryWatchdog::get()};
        auto const armed =
            mm::set_free_memory_signal(0, ~usize{0}, 1, nullptr, mm::DelayedAllocations::Wait);
        if (!armed) {
            FAIL("arm the gate by hand");
        }
        ZoneTrial trial{zone};
        auto const at_low = trial.drain_to(low);

        // An ordinary waiting request, on a machine that is this node alone:
        // the node set is the test's stand-in for the machine, so the request
        // is not confined (it does not ask for Gaf::ThisNode) and every zone
        // it may use is this one. A request that did ask for ThisNode would
        // go on to Min instead (AConfinedWaitingRequestGoesOnToMin).
        auto const ask_waiting = [nid] {
            return mm::priv::alloc_frames(mm::ALLOC_USER_VMO, 0, nid, NodeMask::of(nid));
        };

        // At Low a waiting request is put off; one that cannot wait is served
        // by the second pass.
        auto const waiting = ask_waiting();
        auto const put_off = !waiting && waiting.unwrap_err() == Status::ShouldWait;
        trial.keep(waiting);
        auto const kernel = mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::ThisNode, 0, nid);
        auto const served = kernel && (*kernel)->nid() == nid;
        if (!served) {
            // Not the frame the releases below are counted with: held, so
            // that the trial still gives it back.
            trial.keep(kernel);
        }

        // The zone is one below Low now. Each release below is counted on one
        // CPU's counter, read around it with nothing to preempt it, and the
        // waiting request is tried between them with interrupts on.
        gktl::Counter const reliefs{relief};
        auto const counted = [&reliefs](PmFrame *const block) {
            return count_around(reliefs, [block] { mm::free_frames(block); });
        };

        // Giving that frame back takes the zone to exactly Low, where the
        // waiting request is still refused, so it counts no relief.
        auto early = isize{1};
        auto still_put_off = false;
        if (served) {
            early = counted(*kernel);
            auto const again = ask_waiting();
            still_put_off = !again && again.unwrap_err() == Status::ShouldWait;
            trial.keep(again);
        }

        // The next release crosses Low: it counts one relief, the wake it
        // would send a waiter. Nothing waits here, so the same request is
        // retried on this thread instead, and is served now. One more
        // release, from above Low, counts none.
        auto crossing = isize{0};
        auto served_after = false;
        auto quiet = isize{1};
        if (still_put_off) {
            if (auto *const block = trial.hand_over()) {
                crossing = counted(block);
            }
            auto const retried = ask_waiting();
            served_after = retried && (*retried)->nid() == nid;
            trial.keep(retried);
            if (auto *const block = trial.hand_over()) {
                quiet = counted(block);
            }
        }

        EXPECT_TRUE(at_low && put_off, "at Low a waiting request is answered ShouldWait");
        EXPECT_TRUE(served, "at Low a request that cannot wait is served by the second pass");
        EXPECT_TRUE(early == 0 && still_put_off,
                    "a release back to exactly Low counts no relief, and the request is still put "
                    "off");
        EXPECT_TRUE(crossing == 1 && served_after,
                    "the release that takes the zone above Low counts one relief, and a retried "
                    "waiting request is served");
        EXPECT_TRUE(quiet == 0, "a release from above Low counts no relief");
    }

    KTEST(PmNode, AWaitingRequestGoesOnToMinWithNoWatchdog) {
        // The gate as it is before the watchdog starts, or with none: open,
        // with no delay level. A request put off at Low then has nothing
        // bound to end its wait but a release that need never come, so it is
        // not put off: it goes on to Min, as one that cannot wait does.
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];
        auto const low = zone.watermark(PmZone::Watermark::Low);
        auto const min = zone.watermark(PmZone::Watermark::Min);
        if (low <= min + 1) {
            KTEST_SKIP("the zone's watermarks are too close to test between");
        }

        // While the Hold lives the watchdog arms nothing; when it goes, the
        // watchdog looks and arms the gate again. Before the trial, so that
        // the zone is whole again first.
        mm::MemoryWatchdog::Hold const no_watchdog{mm::MemoryWatchdog::get()};
        auto const disarmed =
            mm::set_free_memory_signal(0, ~usize{0}, 0, nullptr, mm::DelayedAllocations::Wait);
        ZoneTrial trial{zone};

        // The node set stands in for a machine that is this node alone, as in
        // WaitingRequestsStopAtLowAndReliefIsCountedOnce: an ordinary request,
        // which an armed gate would put off here.
        auto const at_low = trial.drain_to(low);
        auto const waiting = mm::priv::alloc_frames(mm::ALLOC_USER_VMO, 0, nid, NodeMask::of(nid));
        auto const served = waiting && (*waiting)->nid() == nid;
        trial.keep(waiting);

        EXPECT_TRUE(disarmed && at_low && served,
                    "with no watchdog to end a wait, a waiting request at Low is served by the "
                    "Min pass");
    }

    KTEST(PmNode, AConfinedWaitingRequestGoesOnToMin) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];
        auto const low = zone.watermark(PmZone::Watermark::Low);
        auto const min = zone.watermark(PmZone::Watermark::Min);
        if (low <= min + 1) {
            KTEST_SKIP("the zone's watermarks are too close to test between");
        }

        // A waiting request confined to the target node: the machine's count
        // says nothing about that node, so nothing would be bound to end a
        // wait there.
        auto const confined = [nid] {
            return mm::alloc_frames(mm::ALLOC_USER_VMO | mm::Gaf::ThisNode, 0, nid);
        };

        ZoneTrial trial{zone};

        // At Low it is not put off: the Min pass serves it from its node.
        auto const at_low = trial.drain_to(low);
        auto const served = confined();
        auto const from_node = served && (*served)->nid() == nid;
        trial.keep(served);

        // At Min it fails, where an ordinary one would have been put off at Low.
        auto const at_min = trial.drain_to(min);
        auto const refused = confined();
        auto const out_of_mem = !refused && refused.unwrap_err() == Status::OutOfMem;
        trial.keep(refused);

        // With no node to use at all -- ThisNode, and a node set without its
        // home -- it fails at once rather than waiting on nothing.
        auto const other = NodeMask::of(mm::current_node());
        auto const nowhere =
            mm::priv::alloc_frames(mm::ALLOC_USER_VMO | mm::Gaf::ThisNode, 0, nid, other);
        auto const nowhere_fails = !nowhere && nowhere.unwrap_err() == Status::OutOfMem;
        trial.keep(nowhere);

        EXPECT_TRUE(at_low && from_node,
                    "at Low a confined waiting request is served by the Min pass, from its node");
        EXPECT_TRUE(at_min && out_of_mem, "at Min it answers OutOfMem, not ShouldWait");
        EXPECT_TRUE(nowhere_fails, "with no node it may use, it answers OutOfMem at once");
    }

    KTEST(PmNode, ABlockCarriesItsLayoutAndCoalescesBack) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];

        // What a block taken and given back in parts must leave as it found:
        // the zone's free-block profile. A quiet node, and the reclaimer
        // paused, so nothing else moves it meanwhile.
        mm::MemoryReclaimer::Pause const no_reclaim{mm::MemoryReclaimer::get()};
        auto const before = zone.free_block_profile();

        constexpr usize ORDER = 3;
        constexpr usize FRAMES = usize{1} << ORDER;
        auto const block = mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::ThisNode, ORDER, nid);
        if (!block) {
            FAIL("an order-{} block from node {}", ORDER, nid);
        }
        auto *const head = *block;
        auto const frame = [head](usize const index) {
            return mm::pfn_to_frame(mm::frame_to_pfn(head) + index);
        };

        auto stamped = head->is_head() && head->order() == ORDER;
        for (usize index = 1; index < FRAMES; ++index) {
            stamped =
                stamped && frame(index)->compound_offset() == index && !frame(index)->is_head();
        }

        // Split into blocks of two: heads at even frames, each tail one away.
        mm::split_block(head, 1);
        auto split_ok = true;
        for (usize index = 0; index < FRAMES; index += 2) {
            split_ok = split_ok && frame(index)->is_head() && frame(index)->order() == 1 &&
                       frame(index + 1)->compound_offset() == 1;
        }

        // The lower two of those again, down to single frames; the upper two
        // stay blocks of two.
        constexpr usize SINGLES = FRAMES / 2;
        mm::split_block(frame(0));
        mm::split_block(frame(2));
        auto singles_ok = true;
        for (usize index = 0; index < SINGLES; ++index) {
            singles_ok = singles_ok && frame(index)->is_head() && frame(index)->order() == 0 &&
                         frame(index)->compound_offset() == 0;
        }
        for (auto index = SINGLES; index < FRAMES; index += 2) {
            singles_ok = singles_ok && frame(index)->order() == 1;
        }

        // Each part goes back through the seam alone. The blocks of two by
        // their heads, then the single frames one by one: the first pair top
        // down, so each frame finds its partner above it, the second bottom
        // up, so each finds it below, and the last one closes the block.
        mm::free_frames(frame(4));
        mm::free_frames(frame(6));
        mm::free_frames(frame(1));
        mm::free_frames(frame(0));
        mm::free_frames(frame(2));
        mm::free_frames(frame(3));
        auto cleared = true;
        for (usize index = 0; index < FRAMES; ++index) {
            cleared = cleared && frame(index)->compound_offset() == 0;
        }
        auto const coalesced = zone.free_block_profile() == before;

        EXPECT_TRUE(stamped, "a block handed out names its head on every tail");
        EXPECT_TRUE(split_ok, "a split stamps each part as a block of its own");
        EXPECT_TRUE(singles_ok,
                    "a part split to order 0 is single frames, and the other parts stay");
        EXPECT_TRUE(cleared, "freed, the parts keep no stamp");
        EXPECT_TRUE(coalesced,
                    "the parts, single frames among them, freed alone coalesce back to where "
                    "the zone was");
    }

    KTEST(PmNode, AdoptedFramesReturnToTheirZone) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto &zone = *target->zone_list().fallback()[0];

        // Eight frames taken from the zone below the pipeline, as memory that
        // was never handed out through it, then given back as two ranges that
        // do not start on the block's boundary. [1, 8) leaves free blocks of
        // one, two and four frames, and [0, 1) joins them into the whole. The
        // buddy coalesces whatever it is given, so the profile cannot tell
        // how the range was carved; what it shows is that adoption reaches
        // the right zone with the right counts. A misaligned block would not
        // get this far: the buddy's release() asserts alignment in every
        // build.
        mm::MemoryReclaimer::Pause const no_reclaim{mm::MemoryReclaimer::get()};
        auto const before = zone.free_block_profile();
        auto block = zone.alloc_frames(3);
        if (!block) {
            FAIL("an order-3 block from zone {}", mm::to_string(zone.zone_type()));
        }
        auto const pfn = mm::frame_to_pfn(&block.value());
        auto const without = zone.free_block_profile();
        auto const taken = without.frames() == before.frames() - 8;

        // The block's own partners are not free, so the three pieces stay as
        // they are, and nothing else in the zone moves.
        mm::adopt_frames(pfn + 1, pfn + 8);
        auto const carved =
            zone.free_block_profile() == without.with_block(0).with_block(1).with_block(2);
        mm::adopt_frames(pfn, pfn + 1);
        auto const whole = zone.free_block_profile() == before;

        EXPECT_TRUE(taken, "the block leaves the zone's count");
        EXPECT_TRUE(carved, "an unaligned range leaves free blocks of one, two and four frames");
        EXPECT_TRUE(whole, "with its last frame adopted, the block is whole again");
    }

    KTEST(PmNode, AContiguousRunGivesItsSurplusBack) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];

        // Three frames aligned to four pages come out of one block of four:
        // the fourth is surplus, and goes back to the zone as the run is made.
        // A quiet node, and the reclaimer paused, so the zone's profile moves
        // only with the run.
        constexpr auto NR_FRAMES = usize{3};
        constexpr auto ALIGN_LOG2 = usize{PAGE_SHIFT + 2};
        mm::MemoryReclaimer::Pause const no_reclaim{mm::MemoryReclaimer::get()};
        auto const before = zone.free_block_profile();
        auto run = mm::alloc_frames_contiguous(mm::ALLOC_KERNEL | mm::Gaf::ThisNode, NR_FRAMES,
                                               ALIGN_LOG2, nid);
        if (!run) {
            FAIL("{} contiguous frames from node {}", NR_FRAMES, nid);
        }
        auto const during = zone.free_block_profile();
        auto from_node = true;
        for (auto &frame: *run) {
            from_node = from_node && frame.nid() == nid;
        }
        // Frame by frame, each a block of its own; the last one finds the
        // surplus free beside it and closes the block of four.
        mm::free_frames(&*run);
        auto const after = zone.free_block_profile();

        EXPECT_TRUE(from_node, "the run comes from the node it names");
        EXPECT_TRUE(during.frames() == before.frames() - NR_FRAMES,
                    "only the run leaves the zone: the surplus frame is free again");
        EXPECT_TRUE(after == before,
                    "the run, freed frame by frame, coalesces back to where the zone was");
    }

    KTEST(PmNode, APausedReclaimerTakesNoNodeRecord) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto &zone = *target->zone_list().fallback()[0];
        auto &reclaimer = mm::MemoryReclaimer::get();

        // While a Pause lives, the thread's take leaves a marked record as it
        // is: a second mark is still not the first. The zone sits above High,
        // so the take's answer is 0 either way; the record is what tells.
        auto marked = false;
        auto refused = usize{1};
        auto kept = false;
        {
            mm::MemoryReclaimer::Pause const no_reclaim{reclaimer};
            auto _ = target->take_reclaim_target();
            marked = target->mark_short(zone);
            refused = reclaimer.take_node_target(*target);
            kept = !target->mark_short(zone);
            auto _ = target->take_reclaim_target();
        }

        // Without one, the take clears it. The thread may take it first,
        // woken as the Pause ended; either way the next mark is the first.
        auto const marked_again = target->mark_short(zone);
        auto _ = reclaimer.take_node_target(*target);
        auto const taken = target->mark_short(zone);
        auto _ = target->take_reclaim_target();

        EXPECT_TRUE(marked && refused == 0 && kept,
                    "while a Pause lives the take leaves a node's record alone");
        EXPECT_TRUE(marked_again && taken, "without a Pause the take clears it");
    }

    KTEST(PmNode, AShortZoneIsMarkedOnceAndTakenOnce) {
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto &zone = *target->zone_list().fallback()[0];
        auto const low = zone.watermark(PmZone::Watermark::Low);
        auto const high = zone.watermark(PmZone::Watermark::High);

        // Paused, so that nothing else marks this node or takes its record
        // while the test reads it: while a Pause lives the reclaimer drops
        // every report and its thread takes no node's record. What an earlier
        // report left is taken first, so that the first mark below is the
        // first. A report that passed its check just before the Pause began
        // could still mark the node; nothing allocates on this node now.
        ZoneTrial trial{zone};
        auto _ = target->take_reclaim_target();

        // Above High: the mark is kept, and asks for nothing.
        auto const above = zone.num_free_frames() >= high;
        auto const first = target->mark_short(zone);
        auto const second = target->mark_short(zone);
        auto const nothing = target->take_reclaim_target();

        // At Low: the distance to High, once.
        auto const at_low = trial.drain_to(low);
        auto const again = target->mark_short(zone);
        auto const wanted = target->take_reclaim_target();
        auto const taken = target->take_reclaim_target();

        EXPECT_TRUE(above && first && !second,
                    "the first mark says it is the first; a second before the take does not");
        EXPECT_TRUE(nothing == 0, "a marked zone at or above High asks for nothing");
        EXPECT_TRUE(at_low && again, "after a take the next mark is the first again");
        EXPECT_TRUE(wanted == high - low, "a marked zone at Low asks for the distance to High");
        EXPECT_TRUE(taken == 0, "the take clears the record");
    }

    KTEST(PmNode, LowZonesKeepAReserve) {
        // The table: every zone's reserve against every highest type is one
        // in LOWMEM_RESERVE_RATIO of what the zones above it on its node
        // manage, up to that type.
        auto table_ok = true;
        PmZone::for_each([&table_ok](PmZone &zone) {
            auto const row = PmZone::row(zone.which_node());
            auto const own = std::to_underlying(zone.zone_type());
            for (usize highest = 0; highest < NR_ZONES_PER_NODE; ++highest) {
                usize above = 0;
                for (auto type = own + 1; type <= highest; ++type) {
                    if (row[type]) {
                        above += row[type]->managed_frames();
                    }
                }
                if (zone.lowmem_reserve(ZoneType(highest)) !=
                    above / PmZone::LOWMEM_RESERVE_RATIO) {
                    table_ok = false;
                }
            }
        });
        EXPECT_TRUE(table_ok, "each zone keeps 1/256 of the memory above it on its node");

        // The admission: a zone that keeps something back from a request that
        // allows Normal. Drained to exactly Low plus that reserve through the
        // zone itself, it refuses such a request and still admits one that
        // allows nothing above it. A low zone, the last any allocation tries,
        // so draining it takes nothing anyone else is about to use. One with
        // more free than that line: on a large machine the Dma zone keeps
        // back more than it has, and cannot be drained down to it.
        PmZone *low = nullptr;
        PmZone::for_each([&low](PmZone &zone) {
            auto const reserve = zone.lowmem_reserve(ZoneType::Normal);
            if (!low && reserve != 0 && zone.num_free_frames() > zone.normal_low_floor()) {
                low = &zone;
            }
        });
        if (!low) {
            KTEST_SKIP("no zone keeps anything back that it can be drained to");
        }
        auto const reserve = low->lowmem_reserve(ZoneType::Normal);
        auto const floor = low->watermark(PmZone::Watermark::Low);

        ZoneTrial trial{*low};
        auto const drained = trial.drain_to(floor + reserve);
        auto const refused = !low->admits(0, mm::AdmissionPass::Low, true, ZoneType::Normal);
        auto const admitted = low->admits(0, mm::AdmissionPass::Low, true, low->zone_type());

        EXPECT_TRUE(drained && refused,
                    "at Low plus its reserve a low zone refuses a request that allows Normal");
        EXPECT_TRUE(admitted, "and still admits a request that allows nothing above it");
    }

    KTEST(PmNode, AllocationsTakeTheHighestZoneFirst) {
        // The zone an ordinary allocation should come from: this node's
        // highest zone with room to spare above the line the first pass
        // admits at, Low plus the reserve the zone keeps from a request that
        // allows Normal. A margin, so that the answer does not change under
        // the test if another CPU allocates meanwhile.
        constexpr auto MARGIN = usize{1024};
        auto const nid = mm::current_node();
        auto *const node = mm::find_node(nid);
        if (!node) {
            FAIL("the calling CPU's node {} is not online", nid);
        }
        PmZone const *expected = nullptr;
        node->zone_list().for_each_local_zone([&expected](PmZone &zone) {
            if (zone.num_free_frames() > zone.normal_low_floor() + MARGIN) {
                expected = &zone;
            }
        });
        if (!expected) {
            KTEST_SKIP("no zone of this node has frames to spare");
        }

        auto const frame = mm::alloc_frames(mm::ALLOC_KERNEL | mm::Gaf::ThisNode, 0, nid);
        if (!frame) {
            FAIL("allocate a frame on node {}", nid);
        }
        EXPECT_TRUE((*frame)->zone() == expected->zone_type(),
                    "an ordinary allocation comes from the highest zone with room, {} here",
                    mm::to_string(expected->zone_type()));
        mm::free_frames(*frame);
    }

    /// The calling thread's placement policy while one lives, and the one it
    /// replaced put back after, however the test ends.
    class PolicyScope {
      public:
        explicit PolicyScope(ustl::Arc<mm::AllocPolicy> policy)
            : m_replaced(mm::set_thread_policy(ustl::move(policy))) {}

        ~PolicyScope() {
            // The policy this scope set, let go of here, outside the lock.
            auto _ = mm::set_thread_policy(ustl::move(m_replaced));
        }

        PolicyScope(PolicyScope const &) = delete;
        auto operator=(PolicyScope const &) -> PolicyScope & = delete;

      private:
        ustl::Arc<mm::AllocPolicy> m_replaced;
    };

    /// The node of one order-0 frame asked for with no node named, or
    /// MAX_NODES when there was none; the frame goes straight back.
    auto node_less_frame_node() -> NodeId {
        auto const frame = mm::alloc_frames(mm::ALLOC_KERNEL);
        if (!frame) {
            return NodeId{MAX_NODES};
        }
        auto const nid = (*frame)->nid();
        mm::free_frames(*frame);
        return nid;
    }

    /// A placement policy of its own, for a PolicyScope: null when there is no
    /// memory for one. The policy is built in place from the factory's value.
    template <typename Make>
    auto new_policy(Make const &make) -> ustl::Arc<mm::AllocPolicy> {
        auto *const policy = new (mm::ALLOC_KERNEL) mm::AllocPolicy(make());
        if (!policy) {
            return {};
        }
        return ustl::Arc<mm::AllocPolicy>::adopt(policy);
    }

    KTEST(PmNode, APlacementPolicyPlacesNodeLessAllocations) {
        using mm::AllocPolicy;

        // A node with memory other than the calling CPU's, for Preferred and
        // Bind: a frame from it is one Default placement would not give. If
        // the thread moves to one of its CPUs meanwhile, the checks still
        // pass and only stop telling the two apart; nothing here can fail
        // for it. And the two lowest nodes with memory, for Interleave, whose
        // order is its own wherever the thread runs.
        auto const local = mm::current_node();
        auto away = NodeId{MAX_NODES};
        auto first = NodeId{MAX_NODES};
        auto second = NodeId{MAX_NODES};
        mm::node_memory_mask().for_each([local, &away, &first, &second](NodeId const nid) {
            if (away == MAX_NODES && nid != local) {
                away = nid;
            }
            if (first == MAX_NODES) {
                first = nid;
            } else if (second == MAX_NODES) {
                second = nid;
            }
        });
        if (second == MAX_NODES || away == MAX_NODES) {
            KTEST_SKIP("needs two nodes with memory");
        }

        // Preferred: its node, for a frame that names none; a frame that
        // names a node is the caller's choice, and the policy leaves it, and
        // takes no turn for it.
        auto preferred_node = NodeId{MAX_NODES};
        auto named_node = NodeId{MAX_NODES};
        if (auto policy = new_policy([away] { return AllocPolicy::preferred(away); })) {
            PolicyScope const scope{ustl::move(policy)};
            preferred_node = node_less_frame_node();
            if (auto const named = mm::alloc_frames(mm::ALLOC_KERNEL, 0, local)) {
                named_node = (*named)->nid();
                mm::free_frames(*named);
            }
        }

        // Bind: only its node, twice, so that three turns in all are taken
        // before the Interleave below: were setting a policy not to start the
        // turns again, it would start on its second node, not its first.
        auto bound_node = NodeId{MAX_NODES};
        auto bound_again = NodeId{MAX_NODES};
        if (auto policy = new_policy([away] { return AllocPolicy::bind(NodeMask::of(away)); })) {
            PolicyScope const scope{ustl::move(policy)};
            bound_node = node_less_frame_node();
            bound_again = node_less_frame_node();
        }

        // Interleave: its nodes in turn, the lower first, one turn per frame.
        std::array<NodeId, 4> interleaved{};
        auto round = NodeMask::of(first);
        round.set(second);
        if (auto policy = new_policy([&round] { return AllocPolicy::interleave(round); })) {
            PolicyScope const scope{ustl::move(policy)};
            for (auto &nid: interleaved) {
                nid = node_less_frame_node();
            }
        }
        auto const restored = !task::Thread::Current::alloc_state().policy();

        EXPECT_TRUE(preferred_node == away,
                    "under Preferred a frame with no node named comes from its node {}", away);
        EXPECT_TRUE(named_node == local,
                    "a frame that names node {} comes from it, whatever the policy prefers", local);
        EXPECT_TRUE(bound_node == away && bound_again == away,
                    "under Bind a frame comes from its node {}", away);
        EXPECT_TRUE(interleaved[0] == first && interleaved[1] == second &&
                        interleaved[2] == first && interleaved[3] == second,
                    "under Interleave frames come from nodes {} and {} in turn, from the first",
                    first, second);
        EXPECT_TRUE(restored, "the thread's policy is none again once the scopes end");
    }

    KTEST(PmNode, ABoundWaitingRequestGoesOnToMin) {
        // A thread bound to a node with one zone, other than the calling
        // CPU's, so that draining it starves nobody: the machine's free count
        // stays healthy, which is what made waiting at Low hang for a request
        // the gate and the watchdog cannot speak for.
        auto *const target = remote_single_zone_node();
        if (!target) {
            KTEST_SKIP("needs a second node with memory in one zone");
        }
        auto const nid = target->nid();
        auto &zone = *target->zone_list().fallback()[0];
        auto const low = zone.watermark(PmZone::Watermark::Low);
        auto const min = zone.watermark(PmZone::Watermark::Min);
        if (low <= min + 1) {
            KTEST_SKIP("the zone's watermarks are too close to test between");
        }
        auto policy = new_policy([nid] { return mm::AllocPolicy::bind(NodeMask::of(nid)); });
        if (!policy) {
            FAIL("a Bind policy for node {}", nid);
        }

        ZoneTrial trial{zone};
        PolicyScope const scope{ustl::move(policy)};
        auto const ask_waiting = [] { return mm::alloc_frames(mm::ALLOC_USER_VMO); };

        // At Low it is not put off: the Min pass serves it from its node.
        auto const at_low = trial.drain_to(low);
        auto const served = ask_waiting();
        auto const from_node = served && (*served)->nid() == nid;
        trial.keep(served);

        // At Min it fails, where an unbound one would wait at Low for a
        // release on a node the machine's count says nothing about.
        auto const at_min = trial.drain_to(min);
        auto const refused = ask_waiting();
        auto const out_of_mem = !refused && refused.unwrap_err() == Status::OutOfMem;
        trial.keep(refused);

        EXPECT_TRUE(at_low && from_node,
                    "at Low a bound waiting request is served by the Min pass, from its node");
        EXPECT_TRUE(at_min && out_of_mem, "at Min it answers OutOfMem, not ShouldWait");
    }

    KTEST(PmNode, MatchesTheMachineItRunsOn) {
        run_topology_checks();
        mm::for_each_node(mm::NodeState::Online, [](PmNode &node) {
            log::info("pmnode: node {}", node.nid());
            check_zone_list(node);
            check_fallback_orders(node.nid());
            check_zone_candidates(node);
            check_contains(node);
        });
        check_fragment_and_bulk();
        check_batch_errors();
        check_node_queries();
        check_count_helpers();
        check_home_allocation();
        check_disabled_allocation();
    }

    // A test of its own, so that the guest it needs -- idle, with a
    // memory-only node -- being absent skips this alone rather than the
    // whole of MatchesTheMachineItRunsOn.
    KTEST(PmNode, FallsBackFromAnExhaustedRemoteNode) {
        if (!ktest::has_option("exhaust-remote")) {
            KTEST_SKIP("needs the exhaust-remote option");
        }
        check_remote_exhaustion();
    }

    KTEST(PmNode, AbsentNodesAnswerNothing) {
        // The first id with no node behind it. A machine that uses every id
        // -- the default build, where MAX_NODES is 1 -- has none to ask.
        auto absent = NodeId{0};
        while (absent < MAX_NODES && mm::find_node(absent)) {
            absent += 1;
        }
        if (absent >= MAX_NODES) {
            KTEST_SKIP("every node id is in use");
        }

        auto const single = mm::alloc_frames(mm::ALLOC_KERNEL, 0, absent);
        auto bulk = mm::alloc_frames_bulk(mm::ALLOC_KERNEL, 1, absent);
        auto fragment = mm::alloc_frames_fragment(mm::ALLOC_KERNEL, 1, absent);
        EXPECT_TRUE(!mm::node_work_cpus(absent).any() && !mm::node_hard_affinity(absent) &&
                        !mm::node_soft_affinity(absent) && !single &&
                        single.unwrap_err() == Status::InvArgs && !bulk &&
                        bulk.unwrap_err() == Status::InvArgs && !fragment &&
                        fragment.unwrap_err() == Status::InvArgs,
                    "queries and allocation handle absent node {}", absent);
        if (single) {
            mm::free_frames(*single);
        }
        if (bulk) {
            mm::free_frames(&*bulk);
        }
        if (fragment) {
            mm::free_frames(&*fragment);
        }
    }
} // namespace
} // namespace ours
