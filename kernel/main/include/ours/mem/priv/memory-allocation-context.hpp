/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PRIV_ALLOCATION_CONTEXT_HPP
#define OURS_MEM_PRIV_ALLOCATION_CONTEXT_HPP 1

#include <ours/mem/types.hpp>
#include <ours/mem/node-mask.hpp>

namespace ours::mem {
    struct MemoryAllocationContext {
        CpuNum initiator;
        NodeMask allowed_nodes;

        NnId home_node;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_PRIV_ALLOCATION_CONTEXT_HPP