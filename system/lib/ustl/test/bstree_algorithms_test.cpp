/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cstddef>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>

namespace {
struct Node final {
    Node *m_parent{};
    Node *m_left{};
    Node *m_right{};
    int m_key{};
};

struct NodeTraits final {
    using NodePtr = Node *;
    using ConstNodePtr = Node const *;
    using Node = ::Node;

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
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Node const *const left, Node const *const right) const noexcept
        -> bool {
        return left->m_key < right->m_key;
    }

    [[nodiscard]]
    constexpr auto operator()(Node const *const left, int const right) const noexcept -> bool {
        return left->m_key < right;
    }

    [[nodiscard]]
    constexpr auto operator()(int const left, Node const *const right) const noexcept -> bool {
        return left < right->m_key;
    }
};

using Algorithms = ustl::collections::intrusive::BstreeAlgorithms<NodeTraits>;
} // namespace

auto main() -> int {
    Node header{};
    Node one{.m_key = 1};
    Node two{.m_key = 2};
    Node three{.m_key = 3};
    Node four{.m_key = 4};
    Algorithms::init_header(&header);

    Algorithms::insert_equal_upper_bound(&header, &two, Compare{});
    Algorithms::insert_equal_upper_bound(&header, &one, Compare{});
    Algorithms::insert_equal_upper_bound(&header, &three, Compare{});
    if (Algorithms::size(&header) != 3U || Algorithms::begin_node(&header) != &one ||
        Algorithms::root_node(&header) != &two || Algorithms::find(&header, 2, Compare{}) != &two ||
        Algorithms::lower_bound(&header, 2, Compare{}) != &two ||
        Algorithms::upper_bound(&header, 2, Compare{}) != &three) {
        return 1;
    }

    Algorithms::InsertCommitData commit_data{};
    const auto [existing, can_insert] =
        Algorithms::insert_unique_check(&header, 4, Compare{}, commit_data);
    if (existing != &three || !can_insert) {
        return 2;
    }
    Algorithms::insert_unique_commit(&header, &four, commit_data);
    if (Algorithms::maximum(Algorithms::root_node(&header)) != &four ||
        Algorithms::count(&header, 3, Compare{}) != 1U) {
        return 3;
    }

    Algorithms::erase(&header, &two);
    if (!Algorithms::inited(&two) || Algorithms::size(&header) != 3U ||
        Algorithms::find(&header, 2, Compare{}) != &header ||
        Algorithms::begin_node(&header) != &one ||
        Algorithms::maximum(Algorithms::root_node(&header)) != &four) {
        return 4;
    }

    Algorithms::unlink(&three);
    if (!Algorithms::inited(&three) || Algorithms::size(&header) != 2U ||
        Algorithms::next_node(&one) != &four || Algorithms::prev_node(&four) != &one) {
        return 5;
    }

    Algorithms::swap_nodes(&one, &four);
    if (Algorithms::begin_node(&header) != &four ||
        Algorithms::maximum(Algorithms::root_node(&header)) != &one ||
        Algorithms::next_node(&four) != &one || Algorithms::prev_node(&one) != &four) {
        return 6;
    }

    Node replacement{.m_key = 4};
    Algorithms::replace_node(&four, &replacement);
    return Algorithms::begin_node(&header) == &replacement &&
                   Algorithms::next_node(&replacement) == &one && Algorithms::size(&header) == 2U
               ? 0
               : 7;
}
