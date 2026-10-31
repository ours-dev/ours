/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <numa/topology.hpp>

#include <algorithm>

namespace numa {
/// Zero until the SRAT parse (or whatever NUMA source there is) fills it; on a
/// machine without one every distance stays zero, and the node order derived
/// from it degenerates to node-id order.
decltype(Topology::DISTANCE) Topology::DISTANCE;

/// MAX_NODES is the "the firmware said nothing" sentinel, and constinit keeps
/// it there from the first instruction: the node layer seeds its own table
/// from this one at bring-up and has to be able to tell silence apart from
/// node 0. Only a topology source writes it -- on x86 the SRAT parse.
constinit decltype(Topology::FIRMWARE_NODE_OF_CPU) Topology::FIRMWARE_NODE_OF_CPU = [] {
    decltype(Topology::FIRMWARE_NODE_OF_CPU) value;
    std::fill(std::begin(value), std::end(value), NodeId(MAX_NODES));
    return value;
}();

} // namespace numa
