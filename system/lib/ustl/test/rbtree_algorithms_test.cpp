/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/rbtree_algorithms.hpp>

#include <array>

#include <cstddef>

namespace {
enum class Color { Red, Black };

struct Node final {
    Node *m_parent{};
    Node *m_left{};
    Node *m_right{};
    Color m_color{Color::Black};
    int m_key{};
};

struct NodeTraits final {
    using Node = ::Node;
    using NodePtr = Node *;
    using ConstNodePtr = Node const *;
    using Color = ::Color;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_parent;
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        node->m_parent = parent;
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_left;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_left = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_right;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_right = right;
    }

    [[nodiscard]]
    static constexpr auto get_color(ConstNodePtr const node) noexcept -> Color {
        return node->m_color;
    }

    static constexpr auto set_color(NodePtr const node, Color const color) noexcept -> void {
        node->m_color = color;
    }

    [[nodiscard]]
    static constexpr auto black() noexcept -> Color {
        return Color::Black;
    }

    [[nodiscard]]
    static constexpr auto red() noexcept -> Color {
        return Color::Red;
    }
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Node const *const left, Node const *const right) const noexcept
        -> bool {
        return left->m_key < right->m_key;
    }
};

using Algorithms = ustl::collections::intrusive::RBTreeAlgorithms<NodeTraits>;

[[nodiscard]]
auto check_subtree(Node const *const node, Node const *const parent) noexcept -> int {
    if (node == nullptr) {
        return 1;
    }
    if (node->m_parent != parent) {
        return -1;
    }
    if (node->m_color == Color::Red &&
        ((node->m_left != nullptr && node->m_left->m_color == Color::Red) ||
         (node->m_right != nullptr && node->m_right->m_color == Color::Red))) {
        return -1;
    }
    const auto left_height = check_subtree(node->m_left, node);
    const auto right_height = check_subtree(node->m_right, node);
    if (left_height < 0 || left_height != right_height) {
        return -1;
    }
    return left_height + (node->m_color == Color::Black ? 1 : 0);
}

[[nodiscard]]
auto is_valid_tree(Node const *const header) noexcept -> bool {
    const auto root = header->m_parent;
    return (root == nullptr && header->m_left == header && header->m_right == header) ||
           (root != nullptr && root->m_parent == header && root->m_color == Color::Black &&
            check_subtree(root, header) > 0);
}
} // namespace

auto main() -> int {
    Node header{};
    Algorithms::init_header(&header);
    std::array<Node, 15U> nodes{};
    constexpr std::array<int, 15U> INSERTION_ORDER{7, 3, 11, 1, 5,  9,  13, 0,
                                                   2, 4, 6,  8, 10, 12, 14};
    for (auto &node: nodes) {
        node.m_key = static_cast<int>(&node - nodes.data());
    }
    for (const auto key: INSERTION_ORDER) {
        Algorithms::insert_equal_upper_bound(&header, &nodes[static_cast<std::size_t>(key)],
                                             Compare{});
        if (!is_valid_tree(&header)) {
            return 1;
        }
    }

    constexpr std::array<int, 15U> ERASURE_ORDER{7, 0, 14, 3, 11, 1, 5, 9, 13, 2, 4, 6, 8, 10, 12};
    for (const auto key: ERASURE_ORDER) {
        const auto node = &nodes[static_cast<std::size_t>(key)];
        static_cast<void>(Algorithms::erase(&header, node));
        if (!Algorithms::inited(node) || !is_valid_tree(&header)) {
            return 2;
        }
    }
    return Algorithms::unique(&header) && Algorithms::size(&header) == 0U ? 0 : 3;
}
