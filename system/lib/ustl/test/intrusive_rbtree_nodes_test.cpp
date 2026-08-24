/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>

#include <ustl/collections/intrusive/detail/rbtree_node.hpp>

using namespace ustl::collections::intrusive;

using NormalTraits = RBTreeNodeTraits<void *, false>;
using CompactTraits = RBTreeNodeTraits<void *, true>;

static_assert(std::same_as<NormalTraits::NodePtr, RBTreeNode<void *> *>);
static_assert(std::same_as<CompactTraits::NodePtr, CompactRbtreeNode<void *> *>);

auto main() -> int {
    NormalTraits::Node root{};
    NormalTraits::Node child{};
    NormalTraits::set_parent(&child, &root);
    NormalTraits::set_left(&root, &child);
    NormalTraits::set_right(&root, &child);
    NormalTraits::set_color(&root, NormalTraits::red());
    if (NormalTraits::get_parent(&child) != &root || NormalTraits::get_left(&root) != &child ||
        NormalTraits::get_right(&root) != &child || NormalTraits::get_color(&root) != NormalTraits::red()) {
        return 1;
    }

    CompactTraits::Node compact_root{};
    CompactTraits::Node compact_child{};
    CompactTraits::set_parent(&compact_child, &compact_root);
    CompactTraits::set_left(&compact_root, &compact_child);
    CompactTraits::set_color(&compact_child, CompactTraits::black());
    if (CompactTraits::get_parent(&compact_child) != &compact_root ||
        CompactTraits::get_left(&compact_root) != &compact_child ||
        CompactTraits::get_color(&compact_child) != CompactTraits::black()) {
        return 2;
    }

    CompactTraits::set_color(&compact_child, CompactTraits::red());
    return CompactTraits::get_parent(&compact_child) == &compact_root &&
                   CompactTraits::get_color(&compact_child) == CompactTraits::red()
               ? 0
               : 3;
}
