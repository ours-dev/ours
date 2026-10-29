/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host tests for the machine's NUMA tables: the distance matrix and the
/// placement the firmware declared. They were only reachable from a booted
/// kernel while this lived inside the page allocator (pmnode.test); two static
/// arrays behind bounds-checked accessors need no kernel at all.
///
/// The running placement is not here. It belongs to the node layer
/// (ours::mm::NodePlacement), which seeds it from this table once and owns it
/// afterwards, and the case that used to check a run-time move leaving the
/// firmware's answer alone went with it. That property is still checked, but
/// only in a booted kernel: pmnode.test's "every possible CPU is still on its
/// firmware node" compares the two tables. A host test for it would have to
/// compile node_placement.cpp, which reaches CpuLocal.
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
            Topology::set_firmware_node(cpu, UNPLACED);
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

TEST_F(TopologyTest, ACpuTheFirmwareDidNotMentionReadsAsUnplaced) {
    EXPECT_EQ(Topology::firmware_node(0), UNPLACED)
        << "silence has to be tellable from node 0, because the node layer "
           "seeds its own table from this one";
}

TEST_F(TopologyTest, TheFirmwarePlacementRoundTrips) {
    Topology::set_firmware_node(1, 2);

    EXPECT_EQ(Topology::firmware_node(1), NodeId{2});
}

TEST_F(TopologyTest, TheFirmwarePlacementIsPerCpu) {
    Topology::set_firmware_node(1, 2);

    EXPECT_EQ(Topology::firmware_node(0), UNPLACED) << "placing one CPU places only that one";
}

} // namespace
