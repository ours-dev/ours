/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>

#include <ustl/collections/intrusive/detail/tree_node.hpp>

using namespace ustl::collections::intrusive;

using Traits = TreeNodeTraits<void *>;

static_assert(std::same_as<Traits::NodePtr, TreeNode<void *> *>);
static_assert(std::same_as<Traits::ConstNodePtr, const TreeNode<void *> *>);

auto main() -> int {
    TreeNode<void *> root{};
    TreeNode<void *> left{};
    TreeNode<void *> right{};

    Traits::set_parent(&left, &root);
    Traits::set_left(&root, &left);
    Traits::set_right(&root, &right);

    if (Traits::get_parent(&left) != &root || Traits::get_left(&root) != &left ||
        Traits::get_right(&root) != &right) {
        return 1;
    }

    const auto *const const_root = &root;
    if (Traits::get_left(const_root) != &left || Traits::get_right(const_root) != &right) {
        return 2;
    }
    return 0;
}
