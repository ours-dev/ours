/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_TREE_NODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_TREE_NODE_HPP 1

#include <ustl/collections/intrusive/pointer_rebind.hpp>

namespace ustl::collections::intrusive {
    template <typename VoidPointer>
    struct TreeNode {
        using NodePtr = typename PointerRebind<VoidPointer, TreeNode>::Type;

        NodePtr m_parent;
        NodePtr m_left;
        NodePtr m_right;
    };

    template <typename VoidPointer>
    struct TreeNodeTraits final {
        using Node = TreeNode<VoidPointer>;
        using NodePtr = typename Node::NodePtr;
        using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

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
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_TREE_NODE_HPP
