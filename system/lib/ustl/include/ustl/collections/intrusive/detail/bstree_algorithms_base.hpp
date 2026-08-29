/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_BSTREE_ALGORITHMS_BASE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_BSTREE_ALGORITHMS_BASE_HPP 1

#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive::detail {
template <typename TNodeTraits>
class BstreeAlgorithmsBase {
  public:
    using Node = typename TNodeTraits::Node;
    using NodeTraits = TNodeTraits;
    using NodePtr = typename TNodeTraits::NodePtr;
    using ConstNodePtr = typename TNodeTraits::ConstNodePtr;

    [[nodiscard]]
    static constexpr auto next_node(NodePtr node) noexcept -> NodePtr {
        const auto right = NodeTraits::get_right(node);
        if (right != NodePtr{}) {
            return minimum(right);
        }
        auto parent = NodeTraits::get_parent(node);
        while (node == NodeTraits::get_right(parent)) {
            node = parent;
            parent = NodeTraits::get_parent(parent);
        }
        return NodeTraits::get_right(node) != parent ? parent : node;
    }

    [[nodiscard]]
    static constexpr auto prev_node(NodePtr node) noexcept -> NodePtr {
        if (is_header(node)) {
            return NodeTraits::get_right(node);
        }
        const auto left = NodeTraits::get_left(node);
        if (left != NodePtr{}) {
            return maximum(left);
        }
        auto parent = NodeTraits::get_parent(node);
        auto current = node;
        while (current == NodeTraits::get_left(parent)) {
            current = parent;
            parent = NodeTraits::get_parent(parent);
        }
        return parent;
    }

    [[nodiscard]]
    static constexpr auto minimum(NodePtr node) noexcept -> NodePtr {
        while (NodeTraits::get_left(node) != NodePtr{}) {
            node = NodeTraits::get_left(node);
        }
        return node;
    }

    [[nodiscard]]
    static constexpr auto maximum(NodePtr node) noexcept -> NodePtr {
        while (NodeTraits::get_right(node) != NodePtr{}) {
            node = NodeTraits::get_right(node);
        }
        return node;
    }

    [[nodiscard]]
    static constexpr auto is_header(ConstNodePtr const node) noexcept -> bool {
        const auto left = NodeTraits::get_left(node);
        const auto right = NodeTraits::get_right(node);
        return NodeTraits::get_parent(node) == NodePtr{} ||
               (left != NodePtr{} && right != NodePtr{} &&
                (left == right || NodeTraits::get_parent(left) != node ||
                 NodeTraits::get_parent(right) != node));
    }

    [[nodiscard]]
    static constexpr auto get_header(ConstNodePtr const node) noexcept -> NodePtr {
        auto current = PointerTraits<NodePtr>::const_cast_from(node);
        auto parent = NodeTraits::get_parent(node);
        if (parent == NodePtr{}) {
            return current;
        }

        auto grandparent = NodeTraits::get_parent(parent);
        if (current != grandparent) {
            do {
                current = parent;
                parent = grandparent;
                grandparent = NodeTraits::get_parent(grandparent);
            } while (current != grandparent);
            return parent;
        }
        return is_header(current) ? current : parent;
    }
};
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_BSTREE_ALGORITHMS_BASE_HPP
