/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_ANY_NODE_AND_ALGORITHMS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_ANY_NODE_AND_ALGORITHMS_HPP 1

#include <ustl/collections/intrusive/pointer_rebind.hpp>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename VoidPointer>
struct AnyNode {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename PointerRebind<VoidPointer, Node>::Type;
    using ConstNodePtr = typename PointerRebind<VoidPointer, Node const>::Type;

    NodePtr m_node_ptr_1{};
    NodePtr m_node_ptr_2{};
    NodePtr m_node_ptr_3{};
    std::size_t m_size_t_1{};
};

template <typename VoidPointer>
struct AnyListNodeTraits final {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;

    [[nodiscard]]
    static constexpr auto get_next(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_1;
    }

    static constexpr auto set_next(NodePtr const node, NodePtr const next) noexcept -> void {
        node->m_node_ptr_1 = next;
    }

    [[nodiscard]]
    static constexpr auto get_previous(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_2;
    }

    static constexpr auto set_previous(NodePtr const node, NodePtr const previous) noexcept
        -> void {
        node->m_node_ptr_2 = previous;
    }
};

template <typename VoidPointer>
struct AnySlistNodeTraits {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;

    [[nodiscard]]
    static constexpr auto get_next(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_1;
    }

    static constexpr auto set_next(NodePtr const node, NodePtr const next) noexcept -> void {
        node->m_node_ptr_1 = next;
    }
};

template <typename VoidPointer>
struct AnyUnorderedNodeTraits final: AnySlistNodeTraits<VoidPointer> {
    using ReducedSlistNodeTraits = AnySlistNodeTraits<VoidPointer>;
    using Node = typename ReducedSlistNodeTraits::Node;
    using NodePtr = typename ReducedSlistNodeTraits::NodePtr;
    using ConstNodePtr = typename ReducedSlistNodeTraits::ConstNodePtr;

    static constexpr bool STORE_HASH = true;
    static constexpr bool OPTIMIZE_MULTIKEY = true;

    [[nodiscard]]
    static constexpr auto get_prev_in_group(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_2;
    }

    static constexpr auto set_prev_in_group(NodePtr const node, NodePtr const previous) noexcept
        -> void {
        node->m_node_ptr_2 = previous;
    }

    [[nodiscard]]
    static constexpr auto get_hash(ConstNodePtr const node) noexcept -> std::size_t {
        return node->m_size_t_1;
    }

    static constexpr auto set_hash(NodePtr const node, std::size_t const hash) noexcept -> void {
        node->m_size_t_1 = hash;
    }
};

template <typename VoidPointer>
struct AnyRbtreeNodeTraits final {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
    using Color = std::size_t;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_1;
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        node->m_node_ptr_1 = parent;
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_2;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_node_ptr_2 = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_3;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_node_ptr_3 = right;
    }

    [[nodiscard]]
    static constexpr auto get_color(ConstNodePtr const node) noexcept -> Color {
        return node->m_size_t_1;
    }

    static constexpr auto set_color(NodePtr const node, Color const color) noexcept -> void {
        node->m_size_t_1 = color;
    }

    [[nodiscard]]
    static constexpr auto black() noexcept -> Color {
        return 0U;
    }

    [[nodiscard]]
    static constexpr auto red() noexcept -> Color {
        return 1U;
    }
};

template <typename VoidPointer>
struct AnyAvltreeNodeTraits final {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
    using Balance = std::size_t;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_1;
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        node->m_node_ptr_1 = parent;
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_2;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_node_ptr_2 = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_3;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_node_ptr_3 = right;
    }

    [[nodiscard]]
    static constexpr auto get_balance(ConstNodePtr const node) noexcept -> Balance {
        return node->m_size_t_1;
    }

    static constexpr auto set_balance(NodePtr const node, Balance const balance) noexcept -> void {
        node->m_size_t_1 = balance;
    }

    [[nodiscard]]
    static constexpr auto negative() noexcept -> Balance {
        return 0U;
    }

    [[nodiscard]]
    static constexpr auto zero() noexcept -> Balance {
        return 1U;
    }

    [[nodiscard]]
    static constexpr auto positive() noexcept -> Balance {
        return 2U;
    }
};

template <typename VoidPointer>
struct AnyTreeNodeTraits final {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_1;
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        node->m_node_ptr_1 = parent;
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_2;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_node_ptr_2 = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_node_ptr_3;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_node_ptr_3 = right;
    }
};

template <typename VoidPointer>
struct AnyNodeTraits final {
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
};

template <typename VoidPointer>
class AnyAlgorithms final {
  public:
    using Node = AnyNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
    using NodeTraits = AnyNodeTraits<VoidPointer>;

    static constexpr auto init(NodePtr const node) noexcept -> void {
        node->m_node_ptr_1 = NodePtr{};
    }

    [[nodiscard]]
    static constexpr auto inited(ConstNodePtr const node) noexcept -> bool {
        return !node->m_node_ptr_1;
    }

    [[nodiscard]]
    static constexpr auto unique(ConstNodePtr const node) noexcept -> bool {
        return !node->m_node_ptr_1;
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_ANY_NODE_AND_ALGORITHMS_HPP
