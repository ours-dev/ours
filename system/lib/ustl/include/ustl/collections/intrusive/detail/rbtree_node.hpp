/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_RBTREE_NODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_RBTREE_NODE_HPP 1

#include <ustl/collections/intrusive/pointer_plus_bits.hpp>
#include <ustl/collections/intrusive/pointer_rebind.hpp>

#include <cstddef>

namespace ustl::collections::intrusive {
template <typename VoidPointer>
struct CompactRbtreeNode {
    using Node = CompactRbtreeNode<VoidPointer>;
    using NodePtr = typename PointerRebind<VoidPointer, Node>::Type;
    using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

    enum class Color { Red, Black };

    NodePtr m_parent;
    NodePtr m_left;
    NodePtr m_right;
};

template <typename VoidPointer>
struct RBTreeNode {
    using Node = RBTreeNode<VoidPointer>;
    using NodePtr = typename PointerRebind<VoidPointer, Node>::Type;
    using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

    enum class Color { Red, Black };

    NodePtr m_parent;
    NodePtr m_left;
    NodePtr m_right;
    Color m_color;
};

template <typename VoidPointer>
struct DefaultRbtreeNodeTraitsImpl {
    using Node = RBTreeNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
    using Color = typename Node::Color;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_parent;
    }

    [[nodiscard]]
    static constexpr auto get_parent(NodePtr const node) noexcept -> NodePtr {
        return node->m_parent;
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        node->m_parent = parent;
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_left;
    }

    [[nodiscard]]
    static constexpr auto get_left(NodePtr const node) noexcept -> NodePtr {
        return node->m_left;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_left = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_right;
    }

    [[nodiscard]]
    static constexpr auto get_right(NodePtr const node) noexcept -> NodePtr {
        return node->m_right;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_right = right;
    }

    [[nodiscard]]
    static constexpr auto get_color(ConstNodePtr const node) noexcept -> Color {
        return node->m_color;
    }

    [[nodiscard]]
    static constexpr auto get_color(NodePtr const node) noexcept -> Color {
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

template <typename VoidPointer>
struct CompactRbtreeNodeTraitsImpl {
    using Node = CompactRbtreeNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename Node::ConstNodePtr;
    using PointerBits = PointerPlusBits<NodePtr, 1>;
    using Color = typename Node::Color;

    [[nodiscard]]
    static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
        return PointerBits::get_pointer(node->m_parent);
    }

    [[nodiscard]]
    static constexpr auto get_parent(NodePtr const node) noexcept -> NodePtr {
        return PointerBits::get_pointer(node->m_parent);
    }

    static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
        PointerBits::set_pointer(node->m_parent, parent);
    }

    [[nodiscard]]
    static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_left;
    }

    [[nodiscard]]
    static constexpr auto get_left(NodePtr const node) noexcept -> NodePtr {
        return node->m_left;
    }

    static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
        node->m_left = left;
    }

    [[nodiscard]]
    static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_right;
    }

    [[nodiscard]]
    static constexpr auto get_right(NodePtr const node) noexcept -> NodePtr {
        return node->m_right;
    }

    static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
        node->m_right = right;
    }

    [[nodiscard]]
    static constexpr auto get_color(ConstNodePtr const node) noexcept -> Color {
        return PointerBits::get_bits(node->m_parent) == 0 ? Color::Black : Color::Red;
    }

    [[nodiscard]]
    static constexpr auto get_color(NodePtr const node) noexcept -> Color {
        return PointerBits::get_bits(node->m_parent) == 0 ? Color::Black : Color::Red;
    }

    static constexpr auto set_color(NodePtr const node, Color const color) noexcept -> void {
        PointerBits::set_bits(node->m_parent, color == Color::Red ? 1 : 0);
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

template <typename VoidPointer, bool Compact>
struct RBTreeNodeTraitsDispatch: DefaultRbtreeNodeTraitsImpl<VoidPointer> {};

template <typename VoidPointer>
struct RBTreeNodeTraitsDispatch<VoidPointer, true>: CompactRbtreeNodeTraitsImpl<VoidPointer> {};

template <typename VoidPointer, bool OptimizeSize = false>
struct RBTreeNodeTraits
    : RBTreeNodeTraitsDispatch<
          VoidPointer,
          OptimizeSize &&
              (MaxPointerPlusBits<VoidPointer, alignof(CompactRbtreeNode<VoidPointer>)>::VALUE >=
               1)> {};

template <typename VoidPointer, bool OptimizeSize = false>
using RBTreeNodeType = typename RBTreeNodeTraits<VoidPointer, OptimizeSize>::Node;
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_RBTREE_NODE_HPP
