/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_AVLTREE_NODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_AVLTREE_NODE_HPP 1

#include <cstddef>

#include <ustl/collections/intrusive/pointer_plus_bits.hpp>
#include <ustl/collections/intrusive/pointer_rebind.hpp>

namespace ustl::collections::intrusive {
    template <typename VoidPointer>
    struct CompactAvltreeNode {
        using Node = CompactAvltreeNode<VoidPointer>;
        using NodePtr = typename PointerRebind<VoidPointer, Node>::Type;
        using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

        enum class Balance { Negative, Zero, Positive };

        NodePtr m_parent;
        NodePtr m_left;
        NodePtr m_right;
    };

    template <typename VoidPointer>
    struct AvltreeNode {
        using Node = AvltreeNode<VoidPointer>;
        using NodePtr = typename PointerRebind<VoidPointer, Node>::Type;
        using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

        enum class Balance { Negative, Zero, Positive };

        NodePtr m_parent;
        NodePtr m_left;
        NodePtr m_right;
        Balance m_balance;
    };

    template <typename VoidPointer>
    struct DefaultAvltreeNodeTraitsImpl {
        using Node = AvltreeNode<VoidPointer>;
        using NodePtr = typename Node::NodePtr;
        using ConstNodePtr = typename Node::ConstNodePtr;
        using Balance = typename Node::Balance;

        [[nodiscard]] static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
            return node->m_parent;
        }

        [[nodiscard]] static constexpr auto get_parent(NodePtr const node) noexcept -> NodePtr {
            return node->m_parent;
        }

        static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
            node->m_parent = parent;
        }

        [[nodiscard]] static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
            return node->m_left;
        }

        [[nodiscard]] static constexpr auto get_left(NodePtr const node) noexcept -> NodePtr {
            return node->m_left;
        }

        static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
            node->m_left = left;
        }

        [[nodiscard]] static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
            return node->m_right;
        }

        [[nodiscard]] static constexpr auto get_right(NodePtr const node) noexcept -> NodePtr {
            return node->m_right;
        }

        static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
            node->m_right = right;
        }

        [[nodiscard]] static constexpr auto get_balance(ConstNodePtr const node) noexcept -> Balance {
            return node->m_balance;
        }

        [[nodiscard]] static constexpr auto get_balance(NodePtr const node) noexcept -> Balance {
            return node->m_balance;
        }

        static constexpr auto set_balance(NodePtr const node, Balance const balance) noexcept -> void {
            node->m_balance = balance;
        }

        [[nodiscard]] static constexpr auto negative() noexcept -> Balance {
            return Balance::Negative;
        }

        [[nodiscard]] static constexpr auto zero() noexcept -> Balance {
            return Balance::Zero;
        }

        [[nodiscard]] static constexpr auto positive() noexcept -> Balance {
            return Balance::Positive;
        }
    };

    template <typename VoidPointer>
    struct CompactAvltreeNodeTraitsImpl {
        using Node = CompactAvltreeNode<VoidPointer>;
        using NodePtr = typename Node::NodePtr;
        using ConstNodePtr = typename Node::ConstNodePtr;
        using PointerBits = PointerPlusBits<NodePtr, 2>;
        using Balance = typename Node::Balance;

        [[nodiscard]] static constexpr auto get_parent(ConstNodePtr const node) noexcept -> NodePtr {
            return PointerBits::get_pointer(node->m_parent);
        }

        [[nodiscard]] static constexpr auto get_parent(NodePtr const node) noexcept -> NodePtr {
            return PointerBits::get_pointer(node->m_parent);
        }

        static constexpr auto set_parent(NodePtr const node, NodePtr const parent) noexcept -> void {
            PointerBits::set_pointer(node->m_parent, parent);
        }

        [[nodiscard]] static constexpr auto get_left(ConstNodePtr const node) noexcept -> NodePtr {
            return node->m_left;
        }

        [[nodiscard]] static constexpr auto get_left(NodePtr const node) noexcept -> NodePtr {
            return node->m_left;
        }

        static constexpr auto set_left(NodePtr const node, NodePtr const left) noexcept -> void {
            node->m_left = left;
        }

        [[nodiscard]] static constexpr auto get_right(ConstNodePtr const node) noexcept -> NodePtr {
            return node->m_right;
        }

        [[nodiscard]] static constexpr auto get_right(NodePtr const node) noexcept -> NodePtr {
            return node->m_right;
        }

        static constexpr auto set_right(NodePtr const node, NodePtr const right) noexcept -> void {
            node->m_right = right;
        }

        [[nodiscard]] static constexpr auto get_balance(ConstNodePtr const node) noexcept -> Balance {
            return static_cast<Balance>(PointerBits::get_bits(node->m_parent));
        }

        [[nodiscard]] static constexpr auto get_balance(NodePtr const node) noexcept -> Balance {
            return static_cast<Balance>(PointerBits::get_bits(node->m_parent));
        }

        static constexpr auto set_balance(NodePtr const node, Balance const balance) noexcept -> void {
            PointerBits::set_bits(node->m_parent, static_cast<std::size_t>(balance));
        }

        [[nodiscard]] static constexpr auto negative() noexcept -> Balance {
            return Balance::Negative;
        }

        [[nodiscard]] static constexpr auto zero() noexcept -> Balance {
            return Balance::Zero;
        }

        [[nodiscard]] static constexpr auto positive() noexcept -> Balance {
            return Balance::Positive;
        }
    };

    template <typename VoidPointer, bool Compact>
    struct AvltreeNodeTraitsDispatch : DefaultAvltreeNodeTraitsImpl<VoidPointer> {};

    template <typename VoidPointer>
    struct AvltreeNodeTraitsDispatch<VoidPointer, true> : CompactAvltreeNodeTraitsImpl<VoidPointer> {};

    template <typename VoidPointer, bool OptimizeSize = false>
    struct AvltreeNodeTraits
        : AvltreeNodeTraitsDispatch<
              VoidPointer,
              OptimizeSize &&
                  (MaxPointerPlusBits<VoidPointer, alignof(CompactAvltreeNode<VoidPointer>)>::VALUE >= 2)> {};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_AVLTREE_NODE_HPP
