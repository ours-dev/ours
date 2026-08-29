/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOCATION_CONTEXT_HPP
#define OURS_MM_PRIV_ALLOCATION_CONTEXT_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/node_mask.hpp>

namespace ours::mm {
    struct MemoryAllocationContext {
        CpuNum initiator;
        NodeMask allowed_nodes;

        NnId home_node;
    };

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ALLOCATION_CONTEXT_HPP