/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/bstree_algorithms_base.hpp>
#include <ustl/collections/intrusive/detail/tree_node.hpp>

namespace {
using Node = ustl::collections::intrusive::TreeNode<void *>;
using Traits = ustl::collections::intrusive::TreeNodeTraits<void *>;
using Algorithms = ustl::collections::intrusive::detail::BstreeAlgorithmsBase<Traits>;
} // namespace

auto main() -> int {
    Node header{};
    Node root{};
    Node left{};
    Node right{};

    Traits::set_parent(&header, &root);
    Traits::set_left(&header, &left);
    Traits::set_right(&header, &right);
    Traits::set_parent(&root, &header);
    Traits::set_left(&root, &left);
    Traits::set_right(&root, &right);
    Traits::set_parent(&left, &root);
    Traits::set_left(&left, nullptr);
    Traits::set_right(&left, nullptr);
    Traits::set_parent(&right, &root);
    Traits::set_left(&right, nullptr);
    Traits::set_right(&right, nullptr);

    return Algorithms::minimum(&root) == &left && Algorithms::maximum(&root) == &right &&
                   Algorithms::next_node(&left) == &root &&
                   Algorithms::next_node(&root) == &right &&
                   Algorithms::next_node(&right) == &header &&
                   Algorithms::prev_node(&header) == &right &&
                   Algorithms::prev_node(&right) == &root && Algorithms::is_header(&header) &&
                   !Algorithms::is_header(&root) && Algorithms::get_header(&left) == &header
               ? 0
               : 1;
}
