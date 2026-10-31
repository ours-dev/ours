/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The ordering derivation, on the host.
///
/// It was a method on the page allocator's node class and could only be
/// checked from a booted kernel, against whatever topology QEMU happened to
/// present -- which on the default image is one node, where every case here
/// collapses. Out here it is a pure function of a matrix the test writes
/// itself, so "nearest first" can be checked on a machine shape no emulator
/// is handing us.
#include <numa/fallback_order.hpp>
#include <numa/topology.hpp>

#include <vector>

#include <gtest/gtest.h>

namespace {
using numa::FallbackOrders;
using numa::MAX_NODES;
using numa::NodeId;
using numa::NodeMask;

/// A row as a vector, so that a case can say what it expects in one line.
auto order_of(FallbackOrders const &orders, NodeId const from) -> std::vector<NodeId> {
    return std::vector<NodeId>(orders[from].begin(), orders[from].end());
}

auto members_of(std::vector<NodeId> const &nids) -> NodeMask {
    NodeMask mask;
    for (auto const nid: nids) {
        mask.set(nid);
    }
    return mask;
}

class FallbackOrderTest: public ::testing::Test {
  protected:
    /// The matrix is one machine-wide table, so a case that left its distances
    /// behind would decide the next case's order.
    auto SetUp() -> void override {
        for (NodeId x = 0; x < MAX_NODES; ++x) {
            for (NodeId y = 0; y < MAX_NODES; ++y) {
                numa::Topology::set_distance(x, y, 0);
            }
        }
    }
};

TEST_F(FallbackOrderTest, NoMembersLeavesEveryRowEmpty) {
    FallbackOrders orders;
    numa::build_fallback_orders(NodeMask{}, NodeMask{}, &orders);

    for (NodeId nid = 0; nid < MAX_NODES; ++nid) {
        EXPECT_TRUE(order_of(orders, nid).empty()) << "node " << nid;
    }
}

TEST_F(FallbackOrderTest, ANodeIsNeverAHopOfItsOwn) {
    FallbackOrders orders;
    auto const set = members_of({0, 1, 2});
    numa::build_fallback_orders(set, set, &orders);

    for (NodeId const nid: {0, 1, 2}) {
        auto const order = order_of(orders, nid);
        EXPECT_EQ(std::count(order.begin(), order.end(), nid), 0) << "node " << nid;
    }
}

TEST_F(FallbackOrderTest, EveryOtherMemberIsListedExactlyOnce) {
    FallbackOrders orders;
    auto const set = members_of({0, 1, 2, 3});
    numa::build_fallback_orders(set, set, &orders);

    auto const order = order_of(orders, 1);
    ASSERT_EQ(order.size(), 3u);
    for (NodeId const other: {0, 2, 3}) {
        EXPECT_EQ(std::count(order.begin(), order.end(), other), 1) << "node " << other;
    }
}

TEST_F(FallbackOrderTest, NearestComesFirst) {
    // From node 0: node 2 is nearer than node 1, which is nearer than node 3.
    numa::Topology::set_distance(0, 1, 20);
    numa::Topology::set_distance(0, 2, 10);
    numa::Topology::set_distance(0, 3, 30);

    FallbackOrders orders;
    auto const set = members_of({0, 1, 2, 3});
    numa::build_fallback_orders(set, set, &orders);

    EXPECT_EQ(order_of(orders, 0), std::vector<NodeId>({2, 1, 3}));
}

TEST_F(FallbackOrderTest, EachRowIsOrderedFromItsOwnNode) {
    // Not one order shared by everyone: 0 and 1 are close, 2 is far from both.
    numa::Topology::set_distance(0, 1, 10);
    numa::Topology::set_distance(0, 2, 30);
    numa::Topology::set_distance(1, 0, 10);
    numa::Topology::set_distance(1, 2, 30);
    numa::Topology::set_distance(2, 0, 30);
    numa::Topology::set_distance(2, 1, 20);

    FallbackOrders orders;
    auto const set = members_of({0, 1, 2});
    numa::build_fallback_orders(set, set, &orders);

    EXPECT_EQ(order_of(orders, 0), std::vector<NodeId>({1, 2}));
    EXPECT_EQ(order_of(orders, 1), std::vector<NodeId>({0, 2}));
    EXPECT_EQ(order_of(orders, 2), std::vector<NodeId>({1, 0}));
}

TEST_F(FallbackOrderTest, AnUnfilledMatrixDegeneratesToNodeIdOrder) {
    // Every distance zero, so every member is equidistant and the tie rule is
    // the only thing deciding. This is the machine the default build boots on,
    // and the property the kernel-side check can see.
    FallbackOrders orders;
    auto const set = members_of({0, 1, 2, 3});
    numa::build_fallback_orders(set, set, &orders);

    EXPECT_EQ(order_of(orders, 2), std::vector<NodeId>({0, 1, 3}));
}

TEST_F(FallbackOrderTest, ANodeOutsideTheRowSetKeepsAnEmptyRow) {
    FallbackOrders orders;
    auto const set = members_of({0, 2});
    numa::build_fallback_orders(set, set, &orders);

    EXPECT_EQ(order_of(orders, 0), std::vector<NodeId>({2}));
    EXPECT_EQ(order_of(orders, 2), std::vector<NodeId>({0}));
    EXPECT_TRUE(order_of(orders, 1).empty());
}

/// The case the two sets exist for, and the one that collapsing them into one
/// argument would have broken: node 1 owns no memory, so it can never be a
/// hop, but it is still a node whose allocations have to fall back somewhere.
TEST_F(FallbackOrderTest, ANodeThatIsNoHopStillGetsAFullRow) {
    numa::Topology::set_distance(1, 0, 30);
    numa::Topology::set_distance(1, 2, 10);

    FallbackOrders orders;
    numa::build_fallback_orders(members_of({0, 1, 2}), members_of({0, 2}), &orders);

    EXPECT_EQ(order_of(orders, 1), std::vector<NodeId>({2, 0}));
    // And it does not list itself, although it is in neither direction a
    // member: `visited` starts holding |from|, whether or not |members| does.
    EXPECT_EQ(order_of(orders, 0), std::vector<NodeId>({2}));
    EXPECT_EQ(order_of(orders, 2), std::vector<NodeId>({0}));
}

} // namespace
