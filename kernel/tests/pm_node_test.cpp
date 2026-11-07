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
///     ever on an even page count. Use the exhaust-remote option from an idle
///     guest with a memory-only NUMA node to check fallback and partial
///     rollback.
///   - Static allocation. Direct class calls honor node restrictions, errors,
///     and allocation-disable state, and release without a node instance.
///   - Count helpers. Both direct-map overloads round three pages up to a
///     usable, zeroed four-page block, including the node-specific template.
///   - AllocState. A thread that disabled allocation must be
///     refused on the node-less path and on the path that names a node alike.
///
/// Why an in-kernel test: PmNode is built on EarlyMem and the buddy, which
/// exist only in a booted kernel.
#include <ours/cpu.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/priv/zone_candidates.hpp>
#include <ours/node_mask.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>
#include <array>
#include <string_view>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/counter.hpp>
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
                  PmNode::num_online_nodes());

        EXPECT_TRUE(nid < MAX_NODES && PmNode::node(nid) != nullptr,
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

        auto const online = PmNode::num_online_nodes();
        EXPECT_TRUE(online >= 1 && online == PmNode::node_online_mask().count(),
                    "num_online_nodes() counts the Online mask");
    }

    /// The checks one node's ZoneList must pass; see the file comment.
    auto check_zone_list(PmNode &node) -> void {
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
        EXPECT_TRUE(by_type_ok && visited == local_count && local_count >= 1,
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
        EXPECT_TRUE(head_ok, "a node's fallback order starts with its own zones");

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
        EXPECT_TRUE(unique_ok, "no zone appears twice in a fallback order");

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
        auto const &online = PmNode::node_online_mask();
        auto const members = online & PmNode::node_memory_mask();

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
        return mm::resolve_request(alloc_flags, 0, PmNode::node_online_mask(),
                                   mm::AllocRequest::Origin{.node = node.nid(), .cpu = Cpu::id()});
    }

    auto check_zone_candidates(PmNode &node) -> void {
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
        mm::ZoneCandidates const holes{list->local_zones(), all};
        EXPECT_EQ(count_selected(holes), local_count,
                  "ZoneCandidates steps over the holes in the by-type array");

        // Node filter: excluding this node leaves only the remote zones.
        auto without_local = all.allowed_nodes();
        without_local.set(node.nid(), false);
        auto const others = all.with_nodes(without_local);
        auto const remote_walk = list->candidates(others);
        auto remote_ok = true;
        usize remote = 0;
        for (auto &zone: remote_walk) {
            if (zone.which_node() == node.nid()) {
                remote_ok = false;
            }
            remote += 1;
        }
        EXPECT_TRUE(remote_ok && remote == list->fallback().size() - local_count,
                    "ZoneCandidates's node mask filters out the excluded node");

        // Upper bound: Dma as the ceiling yields only Dma zones.
        auto const low_only = all.with_zone_range(all.lowest_zone(), ZoneType::Dma);
        auto const low_walk = list->candidates(low_only);
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
        auto const high_walk = list->candidates(high_only);
        auto lower_ok = true;
        usize above = 0;
        for (auto &zone: high_walk) {
            if (zone.zone_type() < ZoneType::Dma32) {
                lower_ok = false;
            }
            above += 1;
        }
        EXPECT_TRUE(lower_ok && above == list->fallback().size() - dma_count,
                    "ZoneCandidates's lower bound keeps a caller off the zones below it");

        // Gaf::ThisNode, through the path that serves it: resolve_request()
        // narrows the request's node mask, so the selector over the whole
        // fallback order yields this node's zones and no others. The walk this
        // replaced switched between two candidate lists instead, which is a
        // second mechanism for the same intent.
        auto const this_node = request_for(node, mm::ALLOC_KERNEL | mm::Gaf::ThisNode);
        auto const local_walk = list->candidates(this_node);
        auto this_node_ok = true;
        usize local_selected = 0;
        for (auto &zone: local_walk) {
            if (zone.which_node() != node.nid()) {
                this_node_ok = false;
            }
            local_selected += 1;
        }
        log::info("pmnode:   ThisNode selected {} of {} zone(s) in the fallback order",
                  local_selected, list->fallback().size());
        EXPECT_TRUE(this_node_ok && local_selected == local_count,
                    "Gaf::ThisNode narrows the request's node mask, and the selector "
                    "honours it");
    }

    auto check_contains(PmNode &node) -> void {
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
        EXPECT_TRUE(bulk_ok && frames == 4, "alloc_frames_bulk() returns n single frames");
    }

    auto check_static_allocation() -> void {
        auto const release = [](ktl::Result<PmFrame *> const &result) {
            if (result && *result) {
                PmNode::free_frames(*result, (*result)->order());
            }
        };
        auto const alloc_flags = mm::ALLOC_KERNEL | mm::Gaf::Zero;
        auto const nid = mm::current_node();

        auto implicit = PmNode::alloc_frames(alloc_flags);
        EXPECT_TRUE(implicit && *implicit && (*implicit)->order() == 0,
                    "static allocation needs no node instance");
        release(implicit);

        auto local = PmNode::alloc_frames(alloc_flags | mm::Gaf::ThisNode);
        EXPECT_TRUE(local && *local, "ThisNode also accepts the implicit home");
        release(local);

        auto explicit_home = PmNode::alloc_frames(nid, alloc_flags | mm::Gaf::ThisNode);
        EXPECT_TRUE(explicit_home && *explicit_home && (*explicit_home)->nid() == nid,
                    "static allocation honors an explicit home and ThisNode");
        release(explicit_home);

        auto empty = PmNode::alloc_frames(nid, alloc_flags, 0, NodeMask{});
        EXPECT_TRUE(!empty && empty.unwrap_err() == Status::OutOfMem,
                    "static allocation cannot escape an empty allowed-node set");
        release(empty);

        auto oversized = PmNode::alloc_frames(nid, alloc_flags, MAX_FRAME_ORDER + 1);
        EXPECT_TRUE(!oversized && oversized.unwrap_err() == Status::InvArgs,
                    "static allocation rejects an order past the maximum");
        release(oversized);

        auto remote_nodes = PmNode::node_memory_mask();
        remote_nodes.set(nid, false);
        if (remote_nodes.any()) {
            auto pinned =
                PmNode::alloc_frames(nid, alloc_flags | mm::Gaf::ThisNode, 0, remote_nodes);
            EXPECT_TRUE(!pinned && pinned.unwrap_err() == Status::OutOfMem,
                        "ThisNode fails when the allowed mask excludes the requested node");
            release(pinned);

            auto remote = PmNode::alloc_frames(nid, alloc_flags, 0, remote_nodes);
            EXPECT_TRUE(remote && *remote && remote_nodes.test((*remote)->nid()),
                        "static allocation reaches another node permitted by the mask");
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
        auto empty_fragment = PmNode::alloc_frames_fragment(NodeId(MAX_NODES), flags, 0);
        auto empty_bulk = PmNode::alloc_frames_bulk(flags, 0);
        EXPECT_TRUE(empty_fragment && empty_fragment->empty() && empty_bulk && empty_bulk->empty(),
                    "zero-page batch requests return empty lists");
        if (empty_fragment) {
            PmNode::free_frames(&*empty_fragment);
        }
        if (empty_bulk) {
            PmNode::free_frames(&*empty_bulk);
        }

        auto fragment = mm::alloc_frames_fragment(flags, PARTIAL_FRAGMENT_COUNT, NodeMask::of(nid));
        EXPECT_TRUE(!fragment && fragment.unwrap_err() == Status::InvArgs,
                    "a fragment request fails when a later block exceeds the maximum order");
        if (fragment) {
            mm::free_frames(&*fragment);
        }

        PmFrame::List list;
        auto const status = mm::alloc_frames(nid, flags, &list, 1, NodeMask{});
        EXPECT_TRUE(status == Status::OutOfMem && list.empty(),
                    "the bulk interface reports an exhausted candidate set");
        mm::free_frames(&list);
    }

    auto check_node_queries() -> void {
        auto const nid = mm::current_node();
        auto *const node = PmNode::node(nid);
        EXPECT_TRUE(node && mm::num_nodes() == PmNode::num_nodes() &&
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
            EXPECT_TRUE(!mm::node_work_cpus(absent).any() && !mm::node_hard_affinity(absent) &&
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

    auto check_local_fallback(NodeId const remote_home) -> bool {
        using Descriptor = gktl::Counter::Descriptor;
        auto const *const hit =
            std::find_if(Descriptor::GLOBAL_DESC_START, Descriptor::GLOBAL_DESC_END,
                         [](Descriptor const &counter) {
                             return std::string_view{counter.name} == "pm.node.local.hit";
                         });
        if (hit == Descriptor::GLOBAL_DESC_END) {
            return false;
        }

        // Read one CPU's counter around the attempt without a preemption or interrupt.
        arch::AutoInterruptDisabler irq;
        auto const local = mm::current_node();
        auto nodes = NodeMask::of(remote_home);
        nodes.set(local);
        gktl::Counter const hits{hit};
        auto const before = hits.get();
        auto const fallback = PmNode::alloc_frames(remote_home, mm::ALLOC_KERNEL, 0, nodes);
        if (!fallback) {
            return false;
        }

        auto const counted_local = (*fallback)->nid() == local && hits.get() == before + 1;
        PmNode::free_frames(*fallback, 0);
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
                auto const block = state.zone.take_block(0);
                auto const count = state.zone.num_free_frames();
                if (block) {
                    state.zone.return_block(block.value(), 0);
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
        auto const mask = NodeMask::of(nid);
        auto const reserved = PmNode::alloc_frames(nid, flags, 0, mask);
        if (!reserved) {
            FAIL("reserve a page on the memory-only node");
        }

        PmFrame::List held;
        for (usize order = MAX_FRAME_ORDER + 1; order > 0; --order) {
            while (auto block = PmNode::alloc_frames(nid, flags, order - 1, mask)) {
                held.push_back(**block);
            }
        }
        auto const pinned = PmNode::alloc_frames(nid, flags);
        auto const pinned_failed = !pinned && pinned.unwrap_err() == Status::OutOfMem;
        if (pinned) {
            PmNode::free_frames(*pinned, 0);
        }
        auto const counted_local = check_local_fallback(nid);

        auto *const last = *reserved;
        auto *const zone = PmZone::at(nid, last->zone());
        auto const drained = zone && zone->count_free_frames() == 0 &&
                             zone->num_free_frames() == 0 &&
                             !zone->watermark_ok(0, PmZone::Watermark::Min);
        PmNode::free_frames(last, 0);
        if (Cpu::num_online_cpus() >= 2) {
            EXPECT_TRUE(drained && check_zone_inventory(*zone),
                        "two CPUs keep an exhausted zone's free count within its one-page "
                        "inventory");
        }
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

        EXPECT_TRUE(pinned_failed, "ThisNode fails after exhausting the requested node");
        EXPECT_TRUE(drained && counted_local,
                    "fallback from the exhausted remote home supplies and counts a local page");
        EXPECT_TRUE(rejected && restored_last, "a partial bulk allocation returns its page");
        EXPECT_TRUE(invalid && fragment_restored_last,
                    "a partial fragment allocation returns its page");
    }

    auto check_remote_exhaustion() -> void {
        // Run from an idle guest. Target a memory-only node so CPUs retain their local memory.
        auto const home = mm::current_node();
        for (NodeId nid = 0; nid < MAX_NODES; ++nid) {
            if (nid != home && PmNode::node_memory_mask().test(nid) &&
                !mm::node_work_cpus(nid).any()) {
                mm::MemoryReclaimer::Pause pause{mm::MemoryReclaimer::get()};
                check_exhausted_node(nid);
                return;
            }
        }
        KTEST_SKIP("remote exhaustion needs a memory-only node");
    }

    auto check_address_helpers() -> void {
        auto const flags = mm::ALLOC_KERNEL;
        auto addr = PhysAddr::Invalid;
        auto const frame = mm::alloc_frames(flags, &addr);
        EXPECT_TRUE(frame && addr == mm::frame_to_phys(*frame),
                    "allocation writes the physical address of the returned frame");
        if (frame) {
            mm::free_frames(mm::frame_to_virt(*frame), 0);
        }

        auto const without_addr = mm::alloc_frames(flags, nullptr);
        EXPECT_TRUE(!!without_addr, "the physical address output is optional");
        if (without_addr) {
            mm::free_frames(*without_addr, 0);
        }

        addr = PhysAddr::Invalid;
        auto const invalid = mm::alloc_frames(flags, &addr, MAX_FRAME_ORDER + 1);
        EXPECT_TRUE(!invalid && invalid.unwrap_err() == Status::InvArgs &&
                        addr == PhysAddr::Invalid,
                    "invalid order is reported and the physical address output is preserved");
        if (invalid) {
            mm::free_frames(*invalid, (*invalid)->order());
        }
    }

    auto check_count_helpers() -> void {
        auto const check = [](u8 *const bytes, NodeId const expected_node, char const *const name) {
            if (!bytes) {
                FAIL("{}", name);
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
            EXPECT_TRUE(ok, "{}", name);
        };

        auto const alloc_flags = mm::ALLOC_KERNEL | mm::Gaf::Zero;
        check(mm::get_frames_n<u8>(alloc_flags, 3), MAX_NODES,
              "get_frames_n() returns a usable zeroed block of at least three pages");
        auto const nid = mm::current_node();
        check(mm::get_frames_n<u8>(nid, alloc_flags | mm::Gaf::ThisNode, 3, NodeMask::of(nid)), nid,
              "node-specific get_frames_n() honors the node and rounds the count");
    }

    auto check_disabled_allocation() -> void {
        auto &state = task::Thread::Current::alloc_state();

        state.disable();
        auto const nodeless = mm::alloc_frames(mm::ALLOC_KERNEL);
        auto const by_node = mm::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL, 0);
        auto direct = PmNode::alloc_frames(mm::ALLOC_KERNEL);
        auto direct_home = PmNode::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL);
        auto const table = mm::alloc_page_table(PAGE_SIZE, PAGE_SIZE);
        auto *const bytes = mm::get_frames<u8>(mm::ALLOC_KERNEL);
        auto *const local_bytes = mm::get_frames<u8>(mm::current_node(), mm::ALLOC_KERNEL);
        state.enable();

        auto const after = mm::alloc_frames(mm::current_node(), mm::ALLOC_KERNEL, 0);
        EXPECT_TRUE(!nodeless && nodeless.unwrap_err() == Status::AccessDenied && !by_node &&
                        by_node.unwrap_err() == Status::AccessDenied,
                    "the facade reports AccessDenied with or without an explicit node");
        EXPECT_TRUE(!!after, "enabling it again lets the allocation through");
        EXPECT_TRUE(table == PhysAddr{0}, "page-table allocation respects disabled allocation");
        if (table != PhysAddr{0}) {
            mm::free_page_table(table, PAGE_SIZE);
        }
        EXPECT_TRUE(!bytes && !local_bytes, "typed allocation returns nullptr when disabled");
        for (auto *const ptr: {bytes, local_bytes}) {
            if (ptr) {
                mm::free_frames(mm::virt_to_frame(ptr), 0);
            }
        }
        EXPECT_TRUE(!direct && direct.unwrap_err() == Status::AccessDenied && !direct_home &&
                        direct_home.unwrap_err() == Status::AccessDenied,
                    "static allocation enforces the thread's disabled state with or without a "
                    "home");
        for (auto const &result: {direct, direct_home}) {
            if (result && *result) {
                PmNode::free_frames(*result, (*result)->order());
            }
        }

        // Freed even if the check above failed, so the test leaks nothing.
        for (auto const &result: {nodeless, by_node, after}) {
            if (result) {
                mm::free_frames(*result, 0);
            }
        }
    }

    KTEST(PmNode, MatchesTheMachineItRunsOn) {
        run_topology_checks();
        PmNode::for_each_node(PmNode::State::Online, [](PmNode &node) {
            log::info("pmnode: node {}", node.nid());
            check_zone_list(node);
            check_fallback_orders(node.nid());
            check_zone_candidates(node);
            check_contains(node);
        });
        check_fragment_and_bulk();
        check_batch_errors();
        check_node_queries();
        if (ktest::has_option("exhaust-remote")) {
            check_remote_exhaustion();
        }
        check_count_helpers();
        check_address_helpers();
        check_static_allocation();
        check_disabled_allocation();
    }
} // namespace
} // namespace ours
