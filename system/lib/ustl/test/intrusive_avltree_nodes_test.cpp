/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/avltree_node.hpp>

#include <concepts>

using namespace ustl::collections::intrusive;

using NormalTraits = AvltreeNodeTraits<void *, false>;
using CompactTraits = AvltreeNodeTraits<void *, true>;

static_assert(std::same_as<NormalTraits::NodePtr, AvltreeNode<void *> *>);
static_assert(std::same_as<CompactTraits::NodePtr, CompactAvltreeNode<void *> *>);

auto main() -> int {
    NormalTraits::Node root{};
    NormalTraits::Node child{};
    NormalTraits::set_parent(&child, &root);
    NormalTraits::set_left(&root, &child);
    NormalTraits::set_balance(&child, NormalTraits::positive());
    if (NormalTraits::get_parent(&child) != &root || NormalTraits::get_left(&root) != &child ||
        NormalTraits::get_balance(&child) != NormalTraits::positive()) {
        return 1;
    }

    CompactTraits::Node compact_root{};
    CompactTraits::Node compact_child{};
    CompactTraits::set_parent(&compact_child, &compact_root);
    CompactTraits::set_right(&compact_root, &compact_child);
    CompactTraits::set_balance(&compact_child, CompactTraits::zero());
    if (CompactTraits::get_parent(&compact_child) != &compact_root ||
        CompactTraits::get_right(&compact_root) != &compact_child ||
        CompactTraits::get_balance(&compact_child) != CompactTraits::zero()) {
        return 2;
    }

    CompactTraits::set_balance(&compact_child, CompactTraits::negative());
    return CompactTraits::get_parent(&compact_child) == &compact_root &&
                   CompactTraits::get_balance(&compact_child) == CompactTraits::negative()
               ? 0
               : 3;
}
