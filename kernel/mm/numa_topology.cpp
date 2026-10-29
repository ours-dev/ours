/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/numa_topology.hpp>

#include <algorithm>

namespace ours::mm {
/// Zero until the SRAT parse (or whatever NUMA source there is) fills it; on a
/// machine without one every distance stays zero, and the fallback order
/// PmNode::Router::build() derives from it degenerates to node-id order.
decltype(NumaTopology::DISTANCE) NumaTopology::DISTANCE;

/// NUMA_MAX_NODE is the "no node known yet" sentinel, and constinit keeps it
/// there from the first instruction: these are read by the allocator before
/// any init hook could fill them. Only a topology source overwrites an entry --
/// the SRAT parse, or the fallback claims in PmNode::bootstrap() and
/// PmNode::Current::claim_possible_cpus() for a CPU nothing else placed.
constinit decltype(NumaTopology::FIRMWARE_CPU_TO_NODE) NumaTopology::FIRMWARE_CPU_TO_NODE = [] {
    decltype(NumaTopology::FIRMWARE_CPU_TO_NODE) value;
    std::fill(std::begin(value), std::end(value), NUMA_MAX_NODE);
    return value;
}();

constinit decltype(NumaTopology::CPU_TO_NODE) NumaTopology::CPU_TO_NODE = [] {
    decltype(NumaTopology::CPU_TO_NODE) value;
    std::fill(std::begin(value), std::end(value), NUMA_MAX_NODE);
    return value;
}();

} // namespace ours::mm
