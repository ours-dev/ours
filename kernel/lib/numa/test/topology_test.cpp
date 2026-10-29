/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host tests for the machine's NUMA tables. They were only reachable from a
/// booted kernel while this lived inside the page allocator (pmnode.test);
/// three static arrays behind bounds-checked accessors need no kernel at all.
///
/// Built with MM_NUMA=1, because MAX_NODES is 1 otherwise and then every case
/// below collapses: one node means the only legal id is 0, so nothing about
/// placement or distance can be told apart.
#include <numa/topology.hpp>

#include <gtest/gtest.h>

namespace {
using numa::Topology;
using ours::CpuNum;
using ours::MAX_NODES;
using ours::NodeId;

/// The tables are static and shared, so each case leaves what it touched back
/// on the sentinel rather than assuming an order between cases.
class TopologyTest: public ::testing::Test {
  protected:
    static constexpr auto UNPLACED = NodeId{MAX_NODES};

    void TearDown() override {
        for (CpuNum cpu = 0; cpu < 4; ++cpu) {
            Topology::set_cpu_node_early(cpu, UNPLACED);
        }
        for (NodeId x = 0; x < 3; ++x) {
            for (NodeId y = 0; y < 3; ++y) {
                Topology::set_distance(x, y, 0);
            }
        }
    }
};

TEST_F(TopologyTest, DistanceStartsAtZeroAndRoundTrips) {
    EXPECT_EQ(Topology::distance(0, 1), 0u) << "a machine with no SLIT has no distances";

    Topology::set_distance(0, 1, 21);
    EXPECT_EQ(Topology::distance(0, 1), 21u);

    // Not symmetric by construction: the firmware states both directions, and
    // nothing here derives one from the other.
    EXPECT_EQ(Topology::distance(1, 0), 0u);
}

TEST_F(TopologyTest, AnUnplacedCpuReadsAsUnplaced) {
    EXPECT_EQ(Topology::cpu_to_node(0), UNPLACED);
    EXPECT_EQ(Topology::firmware_node(0), UNPLACED);
}

TEST_F(TopologyTest, TheEarlyPlacementFillsBothTables) {
    Topology::set_cpu_node_early(1, 2);

    EXPECT_EQ(Topology::cpu_to_node(1), NodeId{2});
    EXPECT_EQ(Topology::firmware_node(1), NodeId{2})
        << "the firmware table is what a node's hard affinity is built from";
}

TEST_F(TopologyTest, ARunTimeMoveLeavesTheFirmwarePlacementAlone) {
    Topology::set_cpu_node_early(1, 2);
    Topology::set_cpu_node(1, 0);

    EXPECT_EQ(Topology::cpu_to_node(1), NodeId{0}) << "the running placement follows the move";
    EXPECT_EQ(Topology::firmware_node(1), NodeId{2})
        << "where the firmware put it does not change when it is moved";
}

TEST_F(TopologyTest, PlacementIsPerCpu) {
    Topology::set_cpu_node_early(1, 2);

    EXPECT_EQ(Topology::cpu_to_node(0), UNPLACED) << "placing one CPU places only that one";
}

} // namespace
