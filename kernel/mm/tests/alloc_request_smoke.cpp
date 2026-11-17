/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for resolve_request(). Mirrors the kernel/mm smoke-test
/// convention: a plain main() that returns a non-zero exit code on the first
/// failing check. No kernel is booted, which is the point of resolving a
/// request through a pure function -- everything it needs is an argument,
/// including the calling CPU.
///
/// Gaf::ThisNode restricts the allowed set to the home node.
/// If the caller's mask excludes it, the request has no candidates.
///
/// Built with MM_NUMA=1, which enables multi-node masks for this host test.
/// The default build has MAX_NODES == 1, and with one node every case
/// below collapses into the same one: the caller's mask, the home node and the
/// result are all {0}, so the narrowing cannot be observed either way. The
/// first version of this test used node ids 1 and 2 against a one-bit mask and
/// "failed" three checks that were really out of range.
#include <ours/mm/priv/alloc_request.hpp>

#include <cstdio>

namespace {
using ours::CpuNum;
using ours::NodeId;
using ours::NodeMask;
using ours::mm::AllocPolicy;
using ours::mm::AllocRequest;
using ours::mm::Gaf;
using ours::mm::resolve_request;
using ours::mm::ZoneType;

auto FAILURES = 0;

auto check(bool const ok, char const *const name) -> void {
    if (ok) {
        std::printf("alloc-request-smoke: pass  %s\n", name);
        return;
    }
    FAILURES += 1;
    std::printf("alloc-request-smoke: FAIL  %s\n", name);
}

constexpr auto HOME = NodeId{1};
constexpr auto OTHER = NodeId{2};
constexpr auto THIRD = NodeId{3};
constexpr auto CPU = CpuNum{3};

/// Deliberately a node id and a CPU number that differ, so that a request
/// which mixed the two would fail a check rather than pass by coincidence.
constexpr auto HERE = AllocRequest::Origin{.node = HOME, .cpu = CPU};

auto mask_of(NodeId const a, NodeId const b) -> NodeMask {
    NodeMask mask;
    mask.set(a);
    mask.set(b);
    return mask;
}

auto carried_through() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const request = resolve_request(Gaf::Zero, 4, nodes, HERE);

    check(request.flags() == Gaf::Zero, "the caller's alloc_flags reaches the request unchanged");
    check(request.order() == 4, "the order reaches the request");
    check(request.home_node() == HOME, "the home node is the one the caller named");
    check(request.initiator() == CPU, "the initiator is the CPU passed in, not one read");
}

auto without_this_node() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const request = resolve_request(Gaf::Zero, 0, nodes, HERE);

    check(request.allowed_nodes().test(HOME) && request.allowed_nodes().test(OTHER),
          "without ThisNode the caller's whole mask is allowed");
}

auto with_this_node_including_home() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const request = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(request.allowed_nodes().test(HOME), "ThisNode keeps the home node");
    check(!request.allowed_nodes().test(OTHER),
          "ThisNode restricts allocation to the allowed home node");
}

auto with_this_node_excluding_home() -> void {
    NodeMask nodes;
    nodes.set(OTHER);
    auto const request = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(!request.allowed_nodes().any(),
          "ThisNode leaves no candidates when the mask excludes the home node");
}

auto empty_mask_stays_empty() -> void {
    NodeMask const nodes;
    auto const request = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(!request.allowed_nodes().any(),
          "an empty mask stays empty: resolving does not invent a node to allow");
}

/// The thread's placement policy, asked for an allocation that names no node.
/// A default policy places it where it was, and Gaf::ThisNode keeps the policy
/// out: the narrowing to the home node is the same with one as without.
auto placement_policy() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    AllocPolicy const policy{};
    auto const placed = resolve_request(Gaf::Zero, 0, nodes, HERE, ustl::some_ref(policy));
    auto const confined = resolve_request(Gaf::ThisNode, 0, nodes, HERE, ustl::some_ref(policy));

    check(placed.home_node() == HOME && placed.allowed_nodes().test(HOME) &&
              placed.allowed_nodes().test(OTHER),
          "a default policy leaves the home node and the caller's mask as they are");
    check(confined.home_node() == HOME && confined.allowed_nodes().test(HOME) &&
              !confined.allowed_nodes().test(OTHER),
          "with ThisNode the policy is not asked: the request is its home node's alone");
    check(placed.placed_by_policy() && !confined.placed_by_policy(),
          "a request the policy placed says so, and takes a turn; one with ThisNode does not");
}

/// Preferred moves the home to its node, when the caller allows that node,
/// and keeps every node the caller allows to fall back to.
auto preferred_policy() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const policy = AllocPolicy::preferred(OTHER);
    auto const placed = resolve_request(Gaf::Zero, 0, nodes, HERE, ustl::some_ref(policy));
    auto const absent = AllocPolicy::preferred(THIRD);
    auto const unplaced = resolve_request(Gaf::Zero, 0, nodes, HERE, ustl::some_ref(absent));

    check(placed.home_node() == OTHER && placed.allowed_nodes() == nodes && !placed.is_confined(),
          "Preferred starts from its node and may fall back to any the caller allows");
    check(unplaced.home_node() == HOME && unplaced.allowed_nodes() == nodes,
          "Preferred leaves the home where it was when the caller does not allow its node");
}

/// Bind narrows the nodes to its own and leaves the home, and the request it
/// narrowed is confined: the machine's free count says nothing about its nodes.
auto bind_policy() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const policy = AllocPolicy::bind(NodeMask::of(OTHER));
    auto const bound = resolve_request(Gaf::CanWait, 0, nodes, HERE, ustl::some_ref(policy));
    auto const wide = AllocPolicy::bind(mask_of(HOME, OTHER));
    auto const unnarrowed = resolve_request(Gaf::CanWait, 0, nodes, HERE, ustl::some_ref(wide));
    auto const elsewhere = AllocPolicy::bind(NodeMask::of(THIRD));
    auto const nowhere = resolve_request(Gaf::Zero, 0, nodes, HERE, ustl::some_ref(elsewhere));

    check(bound.home_node() == HOME && bound.allowed_nodes() == NodeMask::of(OTHER),
          "Bind keeps the home and narrows the nodes to its own");
    check(bound.is_confined() && !bound.waits_at_low(),
          "a request Bind narrowed is confined, and goes on to Min rather than wait");
    check(!unnarrowed.is_confined() && unnarrowed.waits_at_low(),
          "a Bind that leaves every allowed node narrows nothing and confines nothing");
    check(!nowhere.allowed_nodes().any(),
          "a Bind to nodes the caller does not allow leaves nothing to allocate from");
}

/// Interleave moves the home round its allowed nodes, one turn per
/// allocation, lowest id first, and keeps every allowed node to fall back to.
auto interleave_policy() -> void {
    NodeMask nodes = mask_of(HOME, OTHER);
    nodes.set(THIRD);
    auto const policy = AllocPolicy::interleave(mask_of(OTHER, THIRD));
    auto homes_ok = true;
    for (usize turn = 0; turn < 4; ++turn) {
        auto const request =
            resolve_request(Gaf::Zero, 0, nodes, HERE, ustl::some_ref(policy), turn);
        auto expected = OTHER;
        if (turn % 2 == 1) {
            expected = THIRD;
        }
        homes_ok = homes_ok && request.home_node() == expected &&
                   request.allowed_nodes() == nodes && !request.is_confined();
    }
    auto const narrow =
        resolve_request(Gaf::Zero, 0, mask_of(HOME, THIRD), HERE, ustl::some_ref(policy), 0);

    check(homes_ok, "Interleave takes its nodes in turn and may fall back to any allowed");
    check(narrow.home_node() == THIRD,
          "Interleave goes round only the nodes of its own the caller allows");
}

/// Which requests the pipeline treats as confined: those that may use less
/// memory than an ordinary one, by node or by zone type.
auto confinement() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const ordinary = resolve_request(Gaf::Zero, 0, nodes, HERE);
    auto const this_node = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(!ordinary.is_confined(), "an ordinary request is not confined");
    check(this_node.is_confined(), "ThisNode confines a request to its home node");
    check(ordinary.with_zone_range(ZoneType::Dma, ZoneType::Dma32).is_confined(),
          "a zone range that stops below Normal confines a request");
    check(ordinary.with_zone_range(ZoneType::Dma32, ZoneType::Normal).is_confined(),
          "a zone range that starts above Dma confines it too");
    check(!ordinary.with_zone_range(ZoneType::Dma, ZoneType::Movable).is_confined(),
          "a zone range wider than Dma through Normal does not");
}

/// What the request answers about the caller's flags. That the entry hands
/// CanWait on rather than stripping it is the kernel's gate tests' to catch:
/// this test builds requests directly.
auto flag_answers() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const vmo = resolve_request(Gaf::CanBlock | Gaf::CanWait | Gaf::Zero, 0, nodes, HERE);
    auto const confined = resolve_request(Gaf::CanWait | Gaf::ThisNode, 0, nodes, HERE);
    auto const immediate = resolve_request(Gaf{}, 0, nodes, HERE);

    check(vmo.may_block() && vmo.may_wait() && vmo.zeroes(),
          "a VMO page may block, may wait and is zeroed");
    check(vmo.waits_at_low(), "an ordinary request that can wait stops at Low");
    check(confined.may_wait() && !confined.waits_at_low(),
          "a confined request that can wait goes on to Min");
    check(!vmo.with_zone_range(ZoneType::Dma, ZoneType::Dma32).waits_at_low(),
          "so does one whose zone range stops below Normal");
    check(!immediate.may_block() && !immediate.may_wait() && !immediate.zeroes() &&
              !immediate.waits_at_low(),
          "a request with no flags answers no to each");
}

} // namespace

auto main() -> int {
    carried_through();
    without_this_node();
    with_this_node_including_home();
    with_this_node_excluding_home();
    empty_mask_stays_empty();
    placement_policy();
    preferred_policy();
    bind_policy();
    interleave_policy();
    confinement();
    flag_answers();

    if (FAILURES != 0) {
        std::printf("alloc-request-smoke: FAILED -- %d check(s)\n", FAILURES);
        return 1;
    }
    std::printf("alloc-request-smoke: ok\n");
    return 0;
}
