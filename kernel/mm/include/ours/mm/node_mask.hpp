/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_NODE_MASK_HPP
#define OURS_MM_NODE_MASK_HPP 1

#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>

#include <ustl/bitset.hpp>

namespace ours::mm {
using NodeMask = ustl::BitSet<MAX_NODES, NodeId>;
} // namespace ours::mm

#endif // OURS_MM_NODE_MASK_HPP