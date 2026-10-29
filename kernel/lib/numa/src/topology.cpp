/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <numa/topology.hpp>

#include <algorithm>

namespace numa {
/// Zero until the SRAT parse (or whatever NUMA source there is) fills it; on a
/// machine without one every distance stays zero, and the node order derived
/// from it degenerates to node-id order.
decltype(Topology::DISTANCE) Topology::DISTANCE;

/// MAX_NODES is the "no node known yet" sentinel, and constinit keeps it there
/// from the first instruction: these are read before any init hook could fill
/// them. Only a topology source overwrites an entry -- the SRAT parse, or a
/// fallback claim for a CPU nothing else placed.
constinit decltype(Topology::FIRMWARE_NODE_OF_CPU) Topology::FIRMWARE_NODE_OF_CPU = [] {
    decltype(Topology::FIRMWARE_NODE_OF_CPU) value;
    std::fill(std::begin(value), std::end(value), NodeId(MAX_NODES));
    return value;
}();

constinit decltype(Topology::NODE_OF_CPU) Topology::NODE_OF_CPU = [] {
    decltype(Topology::NODE_OF_CPU) value;
    std::fill(std::begin(value), std::end(value), NodeId(MAX_NODES));
    return value;
}();

} // namespace numa
