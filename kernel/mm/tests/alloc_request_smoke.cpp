/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for resolve_request(). Mirrors the kernel/mm smoke-test
/// convention: a plain main() that returns a non-zero exit code on the first
/// failing check. No kernel is booted, which is the point of resolving a
/// request through a pure function -- everything it needs is an argument,
/// including the calling CPU.
///
/// What is worth pinning is the Gaf::ThisNode rule, because it is a
/// restriction and not a preference and the difference is easy to get
/// backwards: it narrows the allowed set to the home node only when the
/// caller's mask already includes that node, and a mask that leaves the home
/// node out keeps its own nodes untouched.
///
/// Built with OURS_CONFIG_NUMA=1, which the target sets for this test alone.
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
using ours::mm::AllocRequest;
using ours::mm::Gaf;
using ours::mm::Origin;
using ours::mm::resolve_request;

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
constexpr auto CPU = CpuNum{3};

/// Deliberately a node id and a CPU number that differ, so that a request
/// which mixed the two would fail a check rather than pass by coincidence.
constexpr auto HERE = Origin{.node = HOME, .cpu = CPU};

auto mask_of(NodeId const a, NodeId const b) -> NodeMask {
    NodeMask mask;
    mask.set(a);
    mask.set(b);
    return mask;
}

auto carried_through() -> void {
    auto const nodes = mask_of(HOME, OTHER);
    auto const request = resolve_request(Gaf::Zero, 4, nodes, HERE);

    check(request.gaf() == Gaf::Zero, "the caller's gaf reaches the request unchanged");
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
          "ThisNode drops every other node: a restriction, not a preference");
}

auto with_this_node_excluding_home() -> void {
    NodeMask nodes;
    nodes.set(OTHER);
    auto const request = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(!request.allowed_nodes().test(HOME),
          "a mask without the home node does not gain it from ThisNode");
    check(request.allowed_nodes().test(OTHER),
          "a mask without the home node keeps its own nodes rather than emptying");
}

auto empty_mask_stays_empty() -> void {
    NodeMask const nodes;
    auto const request = resolve_request(Gaf::ThisNode, 0, nodes, HERE);

    check(!request.allowed_nodes().any(),
          "an empty mask stays empty: resolving does not invent a node to allow");
}

} // namespace

auto main() -> int {
    carried_through();
    without_this_node();
    with_this_node_including_home();
    with_this_node_excluding_home();
    empty_mask_stays_empty();

    if (FAILURES != 0) {
        std::printf("alloc-request-smoke: FAILED -- %d check(s)\n", FAILURES);
        return 1;
    }
    std::printf("alloc-request-smoke: ok\n");
    return 0;
}
